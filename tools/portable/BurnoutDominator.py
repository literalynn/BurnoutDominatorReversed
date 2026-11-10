#!/usr/bin/env python3
"""Burnout Dominator Reversed in one file, for Linux and macOS (Windows: BurnoutDominator.exe).

Put this file in an empty folder and run it with Python 3.11 or later:  python3 BurnoutDominator.py
It downloads the project's latest release next to itself (source/) and starts its launcher, which
keeps the game's files (work/) and the settings in the same folder: the folder can be moved.
The build still needs git, cmake, a C++ compiler and the libraries listed in the README.
"""
import io
import os
from pathlib import Path
import subprocess
import sys
import urllib.request
import zipfile

SOURCE = "https://github.com/literalynn/BurnoutDominatorReversed/releases/latest/download/source.zip"


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
    return subprocess.call([sys.executable, str(source / "tools" / "launcher.py")], env=dict(os.environ, BDR_PORTABLE=str(root)))


if __name__ == "__main__":
    raise SystemExit(main())
