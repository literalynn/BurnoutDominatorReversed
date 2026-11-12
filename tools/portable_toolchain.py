"""Microsoft's C++ compiler, CMake and Ninja in one folder, for a Windows PC without Visual Studio.

The compiler, its C++ runtime and the Windows SDK pieces the game's build needs are packages of the Build
Tools' "Desktop development with C++", the ones the Visual Studio Installer would install: they are read from
Microsoft's channel manifest, downloaded from Microsoft's servers and checked against the SHA-256 the manifest
gives. Nothing is installed: no administrator rights, no registry, nothing outside the folder. Deleting the
folder removes it all. tools/install.py --download-compiler uses it; Python's standard library only.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.parse
import urllib.request
import zipfile

CHANNEL = "https://aka.ms/vs/18/stable/channel"  # Visual Studio 2026, as on the PC the project is developed on
WORKLOAD = "Microsoft.VisualStudio.Workload.VCTools"  # the Build Tools' "Desktop development with C++"
# Under Microsoft.VC.<version>: the x64 compiler for x64 programs and its messages, the C++ library's headers
# and libraries (oldnames.lib is in the Store package), and the runtime DLLs the game gets next to it.
MSVC_PACKAGES = ("Tools.HostX64.TargetX64.base", "Tools.HostX64.TargetX64.Res.base", "CRT.Headers.base",
                 "CRT.x64.Desktop.base", "CRT.x64.Store.base", "CRT.Redist.X64.base")
# The Windows SDK installers with the headers, the x64 libraries, the Universal CRT, rc.exe and mt.exe. The
# headers are spread over several: the "x86" ones hold those of every processor (windowsx.h, dwmapi.h...).
SDK_MSIS = ("Windows SDK for Windows Store Apps Tools", "Windows SDK for Windows Store Apps Headers",
            "Windows SDK for Windows Store Apps Headers OnecoreUap", "Windows SDK Desktop Headers x86",
            "Windows SDK Desktop Headers x64", "Windows SDK OnecoreUap Headers x86", "Windows SDK OnecoreUap Headers x64",
            "Windows SDK for Windows Store Apps Libs", "Windows SDK Desktop Libs x64",
            "Universal CRT Headers Libraries and Sources")
CMAKE = ("https://github.com/Kitware/CMake/releases/download/v4.3.6/cmake-4.3.6-windows-x86_64.zip",
         "570a0d6e1f84fda24b8cb5bb96465b164726c7c066ba44cdadd136ebcb8bf7bd")
NINJA = ("https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
         "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65")
DOWNLOAD_MB = 500  # measured with the packages above (MSVC 14.52, SDK 10.0.26100), manifests included
DISK_BYTES = 1_300_000_000  # 1.25 GB measured once unpacked and pruned
RECORD = "toolchain.json"  # written last: the folder is complete


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "BurnoutDominatorReversed-installer"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def checked(url: str, sha256: str) -> bytes:
    data = fetch(url)
    if hashlib.sha256(data).hexdigest() != sha256.lower():
        raise ValueError(f"{url} does not match its SHA-256. Try again later.")
    return data


def channel_item(channel: dict, name: str) -> dict:
    return next(item for item in channel["channelItems"] if item["id"] == name)


def licence(channel: dict | None = None) -> str:
    """The Build Tools' licence terms, the page the Visual Studio Installer links to."""
    product = channel_item(channel or json.loads(fetch(CHANNEL)), "Microsoft.VisualStudio.Product.BuildTools")
    return next(text["license"] for text in product["localizedResources"] if text["language"].lower() == "en-us")


def package(packages: dict, name: str) -> dict:
    """The language-neutral or English variant of a package."""
    return next(p for p in packages[name.lower()] if p.get("language", "neutral").lower() in ("neutral", "en-us"))


def select(manifest: dict) -> tuple[list[dict], list[dict], list[dict]]:
    """(compiler VSIX payloads, Windows SDK MSI payloads, the SDK's CAB payloads, among which the MSIs pick theirs)."""
    packages: dict[str, list] = {}
    for item in manifest["packages"]:
        packages.setdefault(item["id"].lower(), []).append(item)
    # The compiler version the Installer would choose: the one the default x64 tools depend on.
    tools = package(packages, "Microsoft.VisualCpp.Tools.HostX64.TargetX64")["dependencies"]
    version = next(m[1] for m in map(re.compile(r"Microsoft\.VC\.([\d.]+)\.Tools\.HostX64\.TargetX64\.base", re.I).fullmatch, tools) if m)
    msvc = [p for name in MSVC_PACKAGES for p in package(packages, f"Microsoft.VC.{version}.{name}")["payloads"]]
    # The Windows SDK the workload recommends, as the Installer does.
    workload = package(packages, WORKLOAD)["dependencies"]
    component = next(name for name, kind in workload.items()
                     if re.fullmatch(r"Microsoft\.VisualStudio\.Component\.Windows1[01]SDK\.\d+", name)
                     and isinstance(kind, dict) and kind.get("type") == "Recommended")
    sdk = package(packages, next(iter(package(packages, component)["dependencies"])))["payloads"]
    name = {p["fileName"].replace("\\", "/"): p for p in sdk}
    msis = [name[f"Installers/{msi}-x86_en-us.msi"] for msi in SDK_MSIS]
    cabs = [p for p in sdk if p["fileName"].lower().endswith(".cab")]
    return msvc, msis, cabs


def cabs_of(msi: bytes, cabs: list[dict]) -> list[dict]:
    """The CAB files an MSI names: msiexec reads them from the MSI's folder."""
    return [cab for cab in cabs if Path(cab["fileName"].replace("\\", "/")).name.encode() in msi]


def unzip(data: bytes, target: Path, prefix: str) -> None:
    """The archive's files under `prefix` (a VSIX keeps them in Contents/), written below `target`."""
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for entry in archive.infolist():
            name = urllib.parse.unquote(entry.filename)  # VSIX part names are URL-encoded
            if entry.is_dir() or not name.startswith(prefix):
                continue
            path = (target / name[len(prefix):]).resolve()
            if target.resolve() not in path.parents:
                raise ValueError(f"unsafe path in an archive: {entry.filename}")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(archive.read(entry))


def size(payloads: list[dict]) -> str:
    return f"{sum(p.get('size', 0) for p in payloads) / 1e6:.0f} MB"


def download(folder: Path, say=print) -> None:
    """Fill `folder` (absent or empty) with the compiler, the Windows SDK pieces, CMake and Ninja."""
    channel = json.loads(fetch(CHANNEL))
    # Its own signature covers the package manifest (its SHA-256 in the channel is of another encoding);
    # both come over HTTPS from Microsoft, and every package below is checked against the manifest.
    manifest = json.loads(fetch(channel_item(channel, "Microsoft.VisualStudio.Manifests.VisualStudio")["payloads"][0]["url"]))
    msvc, msis, cabs = select(manifest)
    say(f"Microsoft's licence terms for the Build Tools: {licence(channel)}")
    folder.mkdir(parents=True, exist_ok=True)
    say(f"The C++ compiler ({size(msvc)})")
    for payload in msvc:
        unzip(checked(payload["url"], payload["sha256"]), folder, "Contents/")
    # The compiler starts it to send telemetry; it outlives the build and keeps the folder from being deleted.
    for telemetry in folder.glob("VC/Tools/MSVC/*/bin/Hostx64/x64/vctip.exe"):
        telemetry.unlink()
    installers = folder / "installers"
    installers.mkdir()
    needed = []
    for payload in msis:
        data = checked(payload["url"], payload["sha256"])
        (installers / Path(payload["fileName"].replace("\\", "/")).name).write_bytes(data)
        needed += [cab for cab in cabs_of(data, cabs) if cab not in needed]
    say(f"The Windows SDK ({size(msis + needed)})")
    for payload in needed:
        (installers / Path(payload["fileName"].replace("\\", "/")).name).write_bytes(checked(payload["url"], payload["sha256"]))
    for msi in sorted(installers.glob("*.msi")):
        # An administrative installation only unpacks the files: no elevation, nothing registered.
        code = subprocess.run(f'msiexec.exe /a "{msi}" /qn TARGETDIR="{folder}"').returncode
        if code:
            raise OSError(f"msiexec could not unpack {msi.name} (code {code})")
        (folder / msi.name).unlink(missing_ok=True)  # the copy an administrative installation leaves
    shutil.rmtree(installers)
    # Libraries and tools for other processors, and DirectX debug layers: a third of the SDK's size.
    kits = folder / "Windows Kits" / "10"
    for unused in [*kits.glob("Lib/*/*/x86"), *kits.glob("Lib/*/*/arm64"), *kits.glob("bin/*/x86"), *kits.glob("bin/*/arm64"),
                   kits / "Redist"]:
        shutil.rmtree(unused)
    say("CMake and Ninja")
    unzip(checked(*CMAKE), folder / "cmake", Path(CMAKE[0]).stem + "/")
    unzip(checked(*NINJA), folder / "cmake" / "bin", "")
    msvc_version = next((folder / "VC" / "Tools" / "MSVC").iterdir()).name
    sdk_version = max(p.name for p in (folder / "Windows Kits" / "10" / "Include").iterdir())
    (folder / RECORD).write_text(json.dumps({"msvc": msvc_version, "sdk": sdk_version, "cmake": CMAKE[0], "ninja": NINJA[0]},
                                            indent=2) + "\n", encoding="utf-8")


def ready(folder: Path) -> bool:
    return (folder / RECORD).is_file()


def environment(folder: Path, path: str) -> dict[str, str]:
    """What vcvars64.bat would set for this folder's compiler, with CMake and Ninja put in front of `path`."""
    record = json.loads((folder / RECORD).read_text(encoding="utf-8"))
    msvc, kits, sdk = folder / "VC" / "Tools" / "MSVC" / record["msvc"], folder / "Windows Kits" / "10", record["sdk"]
    cl = msvc / "bin" / "Hostx64" / "x64" / "cl.exe"
    join = lambda paths: os.pathsep.join(str(p) for p in paths)  # noqa: E731
    runtime = sorted({dll.parent for dll in runtime_dlls(folder)})  # for ps2_recomp and the tests the build runs
    return {"PATH": join([cl.parent, kits / "bin" / sdk / "x64", folder / "cmake" / "bin", *runtime])
                    + (os.pathsep + path if path else ""),
            "INCLUDE": join([msvc / "include"] + [kits / "Include" / sdk / part for part in ("ucrt", "shared", "um", "winrt", "cppwinrt")]),
            "LIB": join([msvc / "lib" / "x64", kits / "Lib" / sdk / "ucrt" / "x64", kits / "Lib" / sdk / "um" / "x64"]),
            # CMake takes the compiler from these, whatever other compiler PATH holds.
            "CC": str(cl), "CXX": str(cl)}


def runtime_dlls(folder: Path) -> list[Path]:
    """The C++ runtime DLLs (vcruntime140.dll, msvcp140.dll...) the programs built here need."""
    return sorted((folder / "VC" / "Redist" / "MSVC").glob("*/x64/Microsoft.VC*.CRT/*.dll"))
