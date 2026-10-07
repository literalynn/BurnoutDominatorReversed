#!/usr/bin/env python3
"""Pack what a cloud session needs to rebuild the game and debug its boot.

The ISO (4.6 GB) cannot be uploaded; this zip stays under an upload limit and
unpacks into a work directory (BDR_WORK_DIR):

  local/disc/SYSTEM.CNF, local/disc/<BOOT2 ELF>, local/disc/IOP/*   read from the ISO
  local/analysis/*, local/disc_inventory.json                       ELF/IRX reports, regenerated
  local/analysis/ghidra/functions.ee.csv                            Ghidra map from the work directory
  local/logs/run*.log, recompile.log, augment-function-map.log      previous runs, if present
  generated/generation.json                                         previous generation record
  local/disc/<boot files>                                           files read during the LOADING screen (BOOT_FILES)
  local/disc/<other files>                                          smallest disc files first, while they fit
  bundle.json                                                       manifest

Usage: python tools/cloud_bundle.py --iso <original ISO> [--output <zip>] [--limit-mb 29]
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_disc import run as inventory  # noqa: E402
from iso9660 import ISO9660  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
# Room for zip headers of the entries still to come, the central directory and bundle.json.
RESERVE_BYTES = 512 * 1024
ENTRY_OVERHEAD = 256
WORK_FILES = ["local/analysis/ghidra/functions.ee.csv", "local/analysis/ghidra/ghidra-program-summary.json",
              "local/logs/recompile.log", "local/logs/augment-function-map.log", "generated/generation.json"]
WORK_GLOBS = ["local/logs/run*.log"]
MAX_WORK_FILE = 4 * 1024 * 1024
# Disc files the boot and the front-end loader read during the LOADING screen
# (docs/BOOT.md), larger than most others: included before the smallest-first
# fill so that a cloud session can replay the whole loading phase.
BOOT_FILES = ["DATA/GLOBALE.TXD", "DATA/GLOBALF.TXD", "SOUND/GENERIC.AWD", "SOUND/FE.AWD",
              "FE/LOADING.BIN", "FE/FEMAIN.BIN"]


class BundleError(RuntimeError):
    pass


def default_output() -> Path:
    desktop = Path.home() / "Desktop"
    return (desktop if desktop.is_dir() else Path.cwd()) / "bdr-cloud.zip"


def git_commit() -> str | None:
    try:
        return subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def build_bundle(iso: Path, output: Path, limit_bytes: int, work: Path, lock: dict,
                 boot_files: list[str] = BOOT_FILES) -> dict:
    if not iso.is_file():
        raise BundleError(f"ISO not found: {iso}")
    warnings: list[str] = []
    with tempfile.TemporaryDirectory(prefix="bdr-bundle-") as temp:
        stage = Path(temp)
        local = stage / "local"
        # Selected executables, SYSTEM.CNF, ELF/IRX reports and the disc inventory.
        inventory(iso, local)
        elf = local / "disc" / lock["boot_path"]
        digest = hashlib.sha256(elf.read_bytes()).hexdigest() if elf.is_file() else None
        if digest != lock["elf_sha256"]:
            raise BundleError(f"{iso.name} is not the disc locked in project.json (BOOT2 SHA256 {digest})")

        with ISO9660(iso) as disc:
            # IOPRP300.IMG and any other IOP file that is not an ELF.
            for entry in disc.entries:
                if not entry.is_directory and entry.path.upper().startswith("IOP/"):
                    if not (local / "disc" / entry.path).is_file():
                        disc.extract(entry, local / "disc")

            for relative in WORK_FILES + [p.relative_to(work).as_posix() for g in WORK_GLOBS for p in work.glob(g)]:
                source = work / relative
                if not source.is_file():
                    if relative.endswith("functions.ee.csv"):
                        warnings.append(f"missing {source}: code generation will fall back to heuristics")
                    continue
                if source.stat().st_size > MAX_WORK_FILE:
                    warnings.append(f"skipped {relative}: larger than {MAX_WORK_FILE // (1024 * 1024)} MB")
                    continue
                destination = stage / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source.read_bytes())

            staged = sorted(p for p in stage.rglob("*") if p.is_file())
            included = {p.relative_to(local / "disc").as_posix().casefold()
                        for p in staged if (local / "disc") in p.parents}
            output.parent.mkdir(parents=True, exist_ok=True)
            extra, skipped = [], 0
            with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
                for path in staged:
                    archive.write(path, path.relative_to(stage).as_posix())
                required_bytes = archive.fp.tell()
                if required_bytes + RESERVE_BYTES > limit_bytes:
                    archive.close()
                    output.unlink()
                    raise BundleError(f"required files alone exceed {limit_bytes // (1024 * 1024)} MB")
                first = {name.casefold(): rank for rank, name in enumerate(boot_files)}
                candidates = sorted((e for e in disc.entries
                                     if not e.is_directory and e.path.casefold() not in included),
                                    key=lambda e: (first.get(e.path.casefold(), len(first)), e.size, e.path))
                for entry in candidates:
                    # Worst case: the file does not compress.
                    if archive.fp.tell() + entry.size + ENTRY_OVERHEAD + RESERVE_BYTES > limit_bytes:
                        if entry.path.casefold() in first:
                            continue  # a smaller file after it may still fit
                        break
                    archive.writestr("local/disc/" + entry.path, disc.read(entry))
                    extra.append({"path": entry.path, "size": entry.size, "lba": entry.lba})
                skipped = len(candidates) - len(extra)
                manifest = {
                    "created_utc": datetime.now(timezone.utc).isoformat(),
                    "repository_commit": git_commit(),
                    "iso_name": iso.name, "iso_size": iso.stat().st_size,
                    "boot_path": lock["boot_path"], "elf_sha256": digest,
                    "files": [p.relative_to(stage).as_posix() for p in staged],
                    "required_zip_bytes": required_bytes,
                    "extra_disc_files": extra, "disc_files_not_included": skipped,
                    "warnings": warnings,
                }
                archive.writestr("bundle.json", json.dumps(manifest, indent=2) + "\n")
    size = output.stat().st_size
    if size > limit_bytes:
        raise BundleError(f"{output} is {size} bytes, above the limit")
    return {**manifest, "zip_size": size}


def main() -> int:
    import project  # work directory resolution (BDR_WORK_DIR, work.json)

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", type=Path, help="Original ISO (defaults to the one recorded by extract)")
    parser.add_argument("--output", type=Path, default=default_output())
    parser.add_argument("--limit-mb", type=float, default=29.0)
    args = parser.parse_args()
    iso = args.iso
    paths = project.LOCAL / "paths.json"
    if iso is None and paths.is_file():
        iso = Path(json.loads(paths.read_text(encoding="utf-8"))["iso"])
    if iso is None:
        print("ERROR: pass --iso <path to the original ISO>", file=sys.stderr)
        return 1
    try:
        result = build_bundle(iso.resolve(), args.output.resolve(), int(args.limit_mb * 1024 * 1024),
                              project.WORK, project.LOCK)
    except (BundleError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    for warning in result["warnings"]:
        print("WARNING:", warning)
    print(f"{len(result['files'])} required files, {len(result['extra_disc_files'])} extra disc files "
          f"({result['disc_files_not_included']} too large to fit)")
    print(f"Zip: {args.output.resolve()} ({result['zip_size'] / (1024 * 1024):.1f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
