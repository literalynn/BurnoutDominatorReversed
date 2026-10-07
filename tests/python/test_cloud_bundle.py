"""tools/cloud_bundle.py on a synthetic disc; no copyrighted game bytes are used."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import unittest
import uuid
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from cloud_bundle import ENTRY_OVERHEAD, RESERVE_BYTES, BundleError, build_bundle  # noqa: E402
from test_binary_tools import make_elf, make_iso, record  # noqa: E402


def make_iso_with_iop() -> bytearray:
    """make_iso() plus an IOP/ directory holding a non-ELF IOPRP300.IMG."""
    image = make_iso([record(b"IOP", 25, 2048, 2)])
    iop = b"".join([record(b"\x00", 25, 2048, 2), record(b"\x01", 20, 2048, 2),
                    record(b"IOPRP300.IMG;1", 26, 6)])
    image[25 * 2048:25 * 2048 + len(iop)] = iop
    image[26 * 2048:26 * 2048 + 6] = b"ROMDIR"
    return image


class CloudBundleTests(unittest.TestCase):
    def setUp(self):
        parent = Path(__file__).resolve().parent
        self.root = parent / ("bundle-test-" + uuid.uuid4().hex)
        self.root.mkdir()
        self.root.resolve().relative_to(parent)
        self.addCleanup(shutil.rmtree, self.root)
        self.iso = self.root / "game.iso"
        self.iso.write_bytes(make_iso_with_iop())
        elf = make_elf()
        self.lock = {"boot_path": "SLUS_000.00", "elf_size": len(elf), "elf_sha256": hashlib.sha256(elf).hexdigest()}
        self.work = self.root / "work"
        ghidra = self.work / "local" / "analysis" / "ghidra"
        ghidra.mkdir(parents=True)
        (ghidra / "functions.ee.csv").write_text("Name,Start,End,Size\n", encoding="utf-8")
        logs = self.work / "local" / "logs"
        logs.mkdir()
        (logs / "run3.log").write_text("[BDR] result\n", encoding="utf-8")

    def test_bundle_unpacks_into_a_work_directory(self):
        output = self.root / "out" / "bundle.zip"
        result = build_bundle(self.iso, output, 29 * 1024 * 1024, self.work, self.lock)
        with zipfile.ZipFile(output) as archive:
            names = set(archive.namelist())
            manifest = json.loads(archive.read("bundle.json"))
            self.assertEqual(archive.read("local/disc/IOP/IOPRP300.IMG"), b"ROMDIR")
            self.assertEqual(archive.read("local/disc/ASSET.DAT"), b"plainasset")
        for required in ["local/disc/SYSTEM.CNF", "local/disc/SLUS_000.00", "local/disc_inventory.json",
                         "local/analysis/SLUS_000.00.json", "local/analysis/ghidra/functions.ee.csv",
                         "local/logs/run3.log"]:
            self.assertIn(required, names)
        self.assertTrue(all("\\" not in name for name in names))
        self.assertEqual(manifest["elf_sha256"], self.lock["elf_sha256"])
        self.assertEqual([e["path"] for e in manifest["extra_disc_files"]], ["ASSET.DAT"])
        self.assertEqual(result["disc_files_not_included"], 0)
        self.assertEqual(result["warnings"], [])

    def test_extra_files_stop_at_the_limit(self):
        output = self.root / "small.zip"
        probe = build_bundle(self.iso, output, 29 * 1024 * 1024, self.work, self.lock)
        # The 10-byte extra file needs this much room. The required part varies by a
        # few bytes between runs (timestamps in disc_inventory.json are compressed).
        needed = probe["required_zip_bytes"] + RESERVE_BYTES + ENTRY_OVERHEAD + 10
        result = build_bundle(self.iso, output, needed + 100, self.work, self.lock)
        self.assertEqual([e["path"] for e in result["extra_disc_files"]], ["ASSET.DAT"])
        self.assertLessEqual(output.stat().st_size, needed + 100)
        result = build_bundle(self.iso, output, needed - 100, self.work, self.lock)
        self.assertEqual(result["extra_disc_files"], [])
        self.assertEqual(result["disc_files_not_included"], 1)

    def test_boot_files_come_first_and_never_stop_the_fill(self):
        output = self.root / "boot.zip"
        probe = build_bundle(self.iso, output, 29 * 1024 * 1024, self.work, self.lock)
        needed = probe["required_zip_bytes"] + RESERVE_BYTES + ENTRY_OVERHEAD + 10
        # IOP/IOPRP300.IMG is already required; ASSET.DAT is the only extra file.
        result = build_bundle(self.iso, output, needed + 100, self.work, self.lock,
                              boot_files=["ABSENT.BIN", "ASSET.DAT"])
        self.assertEqual([e["path"] for e in result["extra_disc_files"]], ["ASSET.DAT"])
        # A boot file that does not fit is skipped, not the end of the fill.
        result = build_bundle(self.iso, output, needed - 100, self.work, self.lock, boot_files=["ASSET.DAT"])
        self.assertEqual(result["extra_disc_files"], [])
        self.assertEqual(result["disc_files_not_included"], 1)

    def test_rejects_another_disc_and_reports_missing_map(self):
        with self.assertRaises(BundleError):
            build_bundle(self.iso, self.root / "x.zip", 29 * 1024 * 1024, self.work,
                         {**self.lock, "elf_sha256": "0" * 64})
        (self.work / "local" / "analysis" / "ghidra" / "functions.ee.csv").unlink()
        result = build_bundle(self.iso, self.root / "y.zip", 29 * 1024 * 1024, self.work, self.lock)
        self.assertTrue(any("functions.ee.csv" in w for w in result["warnings"]))

    def test_required_files_over_the_limit_fail(self):
        with self.assertRaises(BundleError):
            build_bundle(self.iso, self.root / "z.zip", 1024, self.work, self.lock)
        self.assertFalse((self.root / "z.zip").exists())


if __name__ == "__main__":
    unittest.main()
