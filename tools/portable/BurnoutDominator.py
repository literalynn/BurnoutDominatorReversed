#!/usr/bin/env python3
"""Burnout Dominator Reversed in one file, for Linux and macOS (Windows: BurnoutDominator.exe).

Put this file in an empty folder and run it with Python 3.11 or later:  python3 BurnoutDominator.py
It downloads the project's latest release next to itself (source/), builds the launcher's window
(source/tools/launcher/, with cmake) and starts it; the launcher keeps the game's files (work/) and
the settings in the same folder: the folder can be moved. Without cmake, or if the window cannot be
built, the launcher opens as a page in the browser instead.
The game's build needs git, cmake, a C++ compiler and the libraries listed in the README.
"""
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

SOURCE = "https://github.com/literalynn/BurnoutDominatorReversed/releases/latest/download/source.zip"
PAGE = "the launcher opens as a page in your browser instead of its own window."


def window(root: Path, source: Path) -> Path | None:
    """The launcher's window, built (or brought up to date) for this copy of the project; None, saying why, when it cannot be."""
    project = source / "tools" / "launcher"
    if os.name == "nt" or not (project / "CMakeLists.txt").is_file():
        return None  # Windows has BurnoutDominator.exe; a project from before the window has only the page
    if not shutil.which("cmake"):
        print(f"cmake was not found: {PAGE}", flush=True)
        return None
    build = root / "work" / "launcher-build"
    configure = ["cmake", "-S", str(project), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"]
    raylib = root / "work" / "deps-src" / "raylib-src"  # the offline copy the game's build uses too, when there is one
    if raylib.is_dir():
        configure.append(f"-DFETCHCONTENT_SOURCE_DIR_RAYLIB={raylib.as_posix()}")
    log = root / "work" / "launcher-build.log"
    for attempt in (1, 2):  # the second from scratch: a folder that was moved leaves a build CMake refuses
        if attempt == 2:
            shutil.rmtree(build, ignore_errors=True)
        if not (build / "bdr_launcher").is_file():
            print("Building the launcher's window, a few minutes...", flush=True)
        log.parent.mkdir(parents=True, exist_ok=True)
        with open(log, "w", encoding="utf-8") as out:
            built = all(subprocess.call(step, stdout=out, stderr=subprocess.STDOUT) == 0
                        for step in (configure, ["cmake", "--build", str(build), "--parallel"]))
        if built and (build / "bdr_launcher").is_file():
            return build / "bdr_launcher"
    print(f"The launcher's window could not be built (what went wrong is in {log}): {PAGE}", flush=True)
    return None


def main() -> int:
    root = Path(__file__).resolve().parent
    source = root / "source"
    if not (source / "tools" / "launcher.py").is_file():
        url = os.environ.get("BDR_SOURCE_URL", SOURCE)  # a fork's release, or a local file to test with
        print(f"First start: downloading {url}", flush=True)
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                archive = zipfile.ZipFile(io.BytesIO(response.read()))
        except (OSError, zipfile.BadZipFile) as error:
            print(f"The download did not go through: {error}", file=sys.stderr)
            return 1
        for name in archive.namelist():
            if (source / name).resolve() != source.resolve() and source.resolve() not in (source / name).resolve().parents:
                print(f"Unsafe path in the archive: {name}", file=sys.stderr)
                return 1
        archive.extractall(source)
    launcher, env = source / "tools" / "launcher.py", dict(os.environ, BDR_PORTABLE=str(root))
    while program := window(root, source):
        code = subprocess.call([str(program), "--python", sys.executable, "--script", str(launcher)], env=env)
        if code == 2:
            print(f"The window could not open (it needs OpenGL 3.3): {PAGE}", flush=True)
            break
        if code != 3:  # 3: an update changed the project, the window is built again
            return code
    return subprocess.call([sys.executable, str(launcher)], env=env)


if __name__ == "__main__":
    raise SystemExit(main())
