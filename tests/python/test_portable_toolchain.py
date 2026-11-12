"""tools/portable_toolchain.py without downloads: package choice, archives, the compiler's environment."""
from __future__ import annotations

import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import portable_toolchain as toolchain  # noqa: E402


def payload(name: str) -> dict:
    return {"fileName": name, "url": "https://example.invalid/" + name, "sha256": "00", "size": 1}


def manifest() -> dict:
    """A Visual Studio manifest cut down to what the choice reads, with versions and variants to leave out."""
    packages = [
        {"id": "Microsoft.VisualCpp.Tools.HostX64.TargetX64",
         "dependencies": {"Microsoft.VC.14.52.Tools.HostX64.TargetX64.base": "14.52.37025"}},
        {"id": "Microsoft.VC.14.44.17.14.Tools.HostX64.TargetX64.base", "payloads": [payload("old.vsix")]},
        {"id": "Microsoft.VisualStudio.Workload.VCTools",
         "dependencies": {"Microsoft.VisualStudio.Component.VC.Tools.x86.x64": {"type": "Recommended"},
                          "Microsoft.VisualStudio.Component.Windows11SDK.28000": {"type": "Optional"},
                          "Microsoft.VisualStudio.Component.Windows11SDK.26100": {"type": "Recommended"}}},
        {"id": "Microsoft.VisualStudio.Component.Windows11SDK.26100", "dependencies": {"Win11SDK_10.0.26100": "10.0.26100.18"}},
        {"id": "Win11SDK_10.0.26100", "payloads": [payload(f"Installers\\{name}-x86_en-us.msi") for name in toolchain.SDK_MSIS]
         + [payload("Installers\\Orca-x86_en-us.msi"), payload("Installers\\aaaa.cab"), payload("Installers\\bbbb.cab")]},
    ]
    for name in toolchain.MSVC_PACKAGES:
        packages.append({"id": f"Microsoft.VC.14.52.{name}", "payloads": [payload(name + ".vsix")]})
    # A package with one variant per language: the English one is taken.
    packages[-1]["language"] = "en-US"
    packages.insert(len(packages) - 1, {**packages[-1], "language": "de-DE", "payloads": [payload("german.vsix")]})
    return {"packages": packages}


class PortableToolchainTests(unittest.TestCase):
    def test_the_installers_choice_of_compiler_and_sdk(self):
        msvc, msis, cabs = toolchain.select(manifest())
        self.assertEqual([p["fileName"] for p in msvc], [name + ".vsix" for name in toolchain.MSVC_PACKAGES])
        self.assertEqual([p["fileName"] for p in msis], [f"Installers\\{name}-x86_en-us.msi" for name in toolchain.SDK_MSIS])
        self.assertEqual([p["fileName"] for p in cabs], ["Installers\\aaaa.cab", "Installers\\bbbb.cab"])
        # An MSI names its cabinets in its tables.
        self.assertEqual(toolchain.cabs_of(b"\0\0Media\0bbbb.cab\0", cabs), [cabs[1]])

    def test_licence_comes_from_the_build_tools_entry(self):
        channel = {"channelItems": [{"id": "Microsoft.VisualStudio.Product.BuildTools", "localizedResources": [
            {"language": "de-de", "license": "https://example.invalid/de"},
            {"language": "en-us", "license": "https://example.invalid/en"}]}]}
        self.assertEqual(toolchain.licence(channel), "https://example.invalid/en")

    def test_archives_keep_their_folder_and_stay_inside_the_target(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("Contents/VC/include/a%20b.h", "x")
            archive.writestr("[Content_Types].xml", "x")
        with tempfile.TemporaryDirectory() as temp:
            toolchain.unzip(data.getvalue(), Path(temp), "Contents/")
            self.assertEqual([p.relative_to(temp).as_posix() for p in Path(temp).rglob("*") if p.is_file()], ["VC/include/a b.h"])
            evil = io.BytesIO()
            with zipfile.ZipFile(evil, "w") as archive:
                archive.writestr("Contents/../../escape.txt", "x")
            with self.assertRaises(ValueError):
                toolchain.unzip(evil.getvalue(), Path(temp) / "inner", "Contents/")

    def test_environment_points_at_the_folder(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            (folder / toolchain.RECORD).write_text(json.dumps({"msvc": "14.52.37025", "sdk": "10.0.26100.0"}), encoding="utf-8")
            crt = folder / "VC" / "Redist" / "MSVC" / "14.52.37025" / "x64" / "Microsoft.VC145.CRT"
            crt.mkdir(parents=True)
            (crt / "vcruntime140.dll").write_bytes(b"")
            env = toolchain.environment(folder, "C:\\Windows")
            msvc = folder / "VC" / "Tools" / "MSVC" / "14.52.37025"
            self.assertEqual(env["PATH"].split(os.pathsep), [str(msvc / "bin" / "Hostx64" / "x64"),
                                                            str(folder / "Windows Kits" / "10" / "bin" / "10.0.26100.0" / "x64"),
                                                            str(folder / "cmake" / "bin"), str(crt), "C:\\Windows"])
            self.assertEqual(env["CXX"], str(msvc / "bin" / "Hostx64" / "x64" / "cl.exe"))
            self.assertIn(str(folder / "Windows Kits" / "10" / "Include" / "10.0.26100.0" / "um"), env["INCLUDE"].split(os.pathsep))
            self.assertEqual(env["LIB"].split(os.pathsep)[0], str(msvc / "lib" / "x64"))
            self.assertEqual(toolchain.runtime_dlls(folder), [crt / "vcruntime140.dll"])


if __name__ == "__main__":
    unittest.main()
