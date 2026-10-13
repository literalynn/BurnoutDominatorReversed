#!/usr/bin/env python3
"""Rebuild a sparse image of the original disc from an unpacked cloud bundle.

tools/cloud_bundle.py --sectors-log packs the disc sectors a traced run read
(local/disc-sectors/<lsn>.bin) next to whole disc files (local/disc/...).
This writes both at their original offsets in a file of the original ISO's
size; every other sector reads as zeros. Running the game with --iso on that
image reproduces the traced run up to the first sector that was not recorded.
No sector is invented: missing data stays zero.

Usage: python tools/sparse_iso.py <bundle directory> [--output <image>]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

SECTOR_SIZE = 2048


def build_sparse_image(bundle: Path, output: Path) -> dict:
    manifest = json.loads((bundle / "bundle.json").read_text(encoding="utf-8"))
    inventory = json.loads((bundle / "local" / "disc_inventory.json").read_text(encoding="utf-8"))
    size = int(manifest["iso_size"])
    lbas = {entry["path"].casefold(): entry for entry in inventory["entries"] if not entry.get("is_directory")}
    disc = bundle / "local" / "disc"
    sectors = bundle / "local" / "disc-sectors"
    written_files, written_runs = [], []
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as image:
        image.truncate(size)
        if disc.is_dir():
            for path in sorted(p for p in disc.rglob("*") if p.is_file()):
                entry = lbas.get(path.relative_to(disc).as_posix().casefold())
                if entry is None:
                    continue  # runtime files such as memory card folders
                data = path.read_bytes()
                if len(data) != entry["size"]:
                    raise ValueError(f"{path} is {len(data)} bytes, the disc says {entry['size']}")
                image.seek(entry["lba"] * SECTOR_SIZE)
                image.write(data)
                written_files.append(entry["path"])
        if sectors.is_dir():
            for path in sorted(sectors.glob("*.bin"), key=lambda p: int(p.stem)):
                lsn = int(path.stem)
                data = path.read_bytes()
                if len(data) % SECTOR_SIZE or lsn * SECTOR_SIZE + len(data) > size:
                    raise ValueError(f"{path}: {len(data)} bytes at sector {lsn} do not fit the image")
                image.seek(lsn * SECTOR_SIZE)
                image.write(data)
                written_runs.append([lsn, len(data) // SECTOR_SIZE])
    return {"image": str(output), "size": size, "files": written_files, "sector_runs": written_runs,
            "sectors_not_included": manifest.get("sectors_not_included", 0)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bundle", type=Path, help="Directory where bdr-cloud.zip was unpacked")
    parser.add_argument("--output", type=Path, help="Image path (default: <bundle>/local/sparse.iso)")
    args = parser.parse_args()
    output = args.output or args.bundle / "local" / "sparse.iso"
    try:
        result = build_sparse_image(args.bundle, output)
    except (OSError, ValueError, KeyError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    sectors = sum(count for _, count in result["sector_runs"])
    print(f"{output}: {len(result['files'])} files and {sectors} recorded sectors written; "
          f"{result['sectors_not_included']} recorded sectors were left out of the bundle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
