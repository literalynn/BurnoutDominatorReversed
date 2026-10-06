"""Synthetic parser fixtures; no copyrighted game bytes are used in tests."""
from __future__ import annotations

import os
from pathlib import Path
import struct
import sys
import shutil
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from analyze_disc import run
from elf32 import ELFError, UnsupportedELFError, analyze_elf, printable_strings
from iso9660 import DiscError, ISO9660, UnsupportedDiscError, both16, both32, clean_identifier, parse_boot2, safe_destination


def make_elf(endian="<", symbols=False, extended=False):
    data = bytearray(1024)
    data[:16] = b"\x7fELF" + bytes([1, 1 if endian == "<" else 2, 1]) + bytes(9)
    names = b"\x00.text\x00.shstrtab\x00.strtab\x00.symtab\x00"
    section_count = 5 if symbols else 3
    shoff = 512
    struct.pack_into(endian + "HHIIIIIHHHHHH", data, 16,
                     2, 8, 1, 0x100000, 52, shoff, 0, 52, 32,
                     0xFFFF if extended else 1, 40, 0 if extended else section_count,
                     0xFFFF if extended else 2)
    struct.pack_into(endian + "8I", data, 52, 1, 256, 0x100000, 0x100000, 8, 16, 5, 256)
    struct.pack_into(endian + "II", data, 256, 0x03E00008, 0)
    data[300:300 + len(names)] = names
    sections = [(0, 0, 0, 0, 0, section_count if extended else 0, 2 if extended else 0, 1 if extended else 0, 0, 0),
                (1, 1, 6, 0x100000, 256, 8, 0, 0, 4, 0),
                (7, 3, 0, 0, 300, len(names), 0, 0, 1, 0)]
    if symbols:
        data[400:406] = b"\x00main\x00"
        struct.pack_into(endian + "IIIBBH", data, 432, 1, 0x100000, 8, 0x12, 0, 1)
        sections.extend([(17, 3, 0, 0, 400, 6, 0, 0, 1, 0),
                         (25, 2, 0, 0, 416, 32, 3, 1, 4, 16)])
    for i, fields in enumerate(sections):
        struct.pack_into(endian + "10I", data, shoff + i * 40, *fields)
    return data


def record(identifier: bytes, lba: int, size: int, flags=0):
    length = 33 + len(identifier) + (len(identifier) % 2 == 0)
    result = bytearray(length)
    result[0] = length
    struct.pack_into("<I", result, 2, lba)
    struct.pack_into(">I", result, 6, lba)
    struct.pack_into("<I", result, 10, size)
    struct.pack_into(">I", result, 14, size)
    result[25] = flags
    result[28:32] = b"\x01\x00\x00\x01"
    result[32] = len(identifier)
    result[33:33 + len(identifier)] = identifier
    return result


def make_iso(extra_records=None):
    image = bytearray(32 * 2048)
    pvd = memoryview(image)[16 * 2048:17 * 2048]
    pvd[0:7] = b"\x01CD001\x01"
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[40:72] = b"SYNTHETIC_TEST".ljust(32)
    pvd[80:88] = struct.pack("<I", 32) + struct.pack(">I", 32)
    pvd[128:132] = struct.pack("<H", 2048) + struct.pack(">H", 2048)
    root = record(b"\x00", 20, 2048, 2)
    pvd[156:156 + len(root)] = root
    image[17 * 2048:17 * 2048 + 7] = b"\xffCD001\x01"
    cnf = b"BOOT2 = cdrom0:\\SLUS_000.00;1\r\nVER = 1.00\r\n"
    elf = make_elf()
    records = [root, record(b"\x01", 20, 2048, 2), record(b"SYSTEM.CNF;1", 21, len(cnf)),
               record(b"SLUS_000.00;1", 22, len(elf)), record(b"RAWBIN;1", 23, len(elf)),
               record(b"ASSET.DAT;1", 24, 10)]
    if extra_records:
        records.extend(extra_records)
    directory = b"".join(records)
    image[20 * 2048:20 * 2048 + len(directory)] = directory
    image[21 * 2048:21 * 2048 + len(cnf)] = cnf
    image[22 * 2048:22 * 2048 + len(elf)] = elf
    image[23 * 2048:23 * 2048 + len(elf)] = elf
    image[24 * 2048:24 * 2048 + 10] = b"plainasset"
    return image


class ELFTests(unittest.TestCase):
    def test_little_and_big_endian_symbols_and_words(self):
        for endian, expected in (("<", "little"), (">", "big")):
            with self.subTest(endian=endian):
                result = analyze_elf(make_elf(endian, symbols=True))
                self.assertEqual(result["header"]["endian"], expected)
                self.assertEqual(result["entry_location"]["instruction_words"], ["0x03e00008", "0x00000000"])
                self.assertEqual(result["named_function_symbol_count"], 1)
                self.assertEqual(result["symbols"][1]["name"], "main")
                self.assertFalse(result["stripped"])

    def test_extended_section_and_program_counts(self):
        result = analyze_elf(make_elf(symbols=True, extended=True))
        self.assertEqual(result["header"]["resolved_section_count"], 5)
        self.assertEqual(result["header"]["resolved_program_count"], 1)
        self.assertEqual(result["sections"][2]["name"], ".shstrtab")

    def test_malformed_header_and_unsupported_class(self):
        with self.assertRaises(ELFError):
            analyze_elf(b"\x7fELF")
        data = make_elf()
        data[4] = 2
        with self.assertRaises(UnsupportedELFError):
            analyze_elf(data)
        data[4] = 1
        data[5] = 0
        with self.assertRaises(ELFError):
            analyze_elf(data)

    def test_tables_and_segments_out_of_bounds(self):
        data = make_elf()
        struct.pack_into("<I", data, 28, 1000)
        with self.assertRaisesRegex(ELFError, "program table"):
            analyze_elf(data)
        data = make_elf()
        struct.pack_into("<I", data, 52 + 16, 2048)
        with self.assertRaisesRegex(ELFError, "program 0"):
            analyze_elf(data)
        data = make_elf()
        struct.pack_into("<I", data, 512 + 40 + 20, 2048)
        with self.assertRaisesRegex(ELFError, "section 1"):
            analyze_elf(data)

    def test_load_memory_size_and_address_overflow(self):
        data = make_elf()
        struct.pack_into("<I", data, 52 + 20, 4)
        with self.assertRaisesRegex(ELFError, "file size exceeds memory"):
            analyze_elf(data)
        data = make_elf()
        struct.pack_into("<I", data, 52 + 8, 0xFFFFFFF8)
        with self.assertRaisesRegex(ELFError, "overflows"):
            analyze_elf(data)

    def test_ps2_nonconforming_alignment_is_reported(self):
        data = make_elf()
        struct.pack_into("<I", data, 52 + 8, 0x100080)
        result = analyze_elf(data)
        self.assertTrue(any("alignment disagrees" in w for w in result["warnings"]))

    def test_symbol_link_and_entry_size_validation(self):
        data = make_elf(symbols=True)
        struct.pack_into("<I", data, 512 + 4 * 40 + 24, 99)
        with self.assertRaisesRegex(ELFError, "invalid string table"):
            analyze_elf(data)
        data = make_elf(symbols=True)
        struct.pack_into("<I", data, 512 + 4 * 40 + 36, 15)
        with self.assertRaisesRegex(ELFError, "symbol entry size"):
            analyze_elf(data)

    def test_unterminated_strings_and_symbol_section_validation(self):
        data = make_elf(symbols=True)
        data[405] = 65
        with self.assertRaisesRegex(ELFError, "not NUL-terminated"):
            analyze_elf(data)
        data = make_elf(symbols=True)
        struct.pack_into("<H", data, 432 + 14, 30)
        with self.assertRaisesRegex(ELFError, "symbol section index"):
            analyze_elf(data)

    def test_nobits_does_not_need_file_bytes(self):
        data = make_elf()
        struct.pack_into("<I", data, 512 + 40 + 4, 8)
        struct.pack_into("<I", data, 512 + 40 + 16, 0xFFFFFFFF)
        analyze_elf(data)

    def test_printable_strings_handles_trailing_text(self):
        self.assertEqual(printable_strings(b"\x00hello\x00world", 5),
                         [{"offset": 1, "text": "hello"}, {"offset": 7, "text": "world"}])


class ISOTests(unittest.TestCase):
    def setUp(self):
        fixture_parent = Path(__file__).resolve().parent
        self.root = fixture_parent / ("parser-test-" + uuid.uuid4().hex)
        self.root.mkdir()
        # Windows Python's private temp ACLs may exclude the sandbox identity.
        # Use inherited permissions, and verify scope before recursive cleanup.
        self.root.resolve().relative_to(fixture_parent)
        self.addCleanup(shutil.rmtree, self.root)

    def write_iso(self, data):
        path = self.root / "test.iso"
        path.write_bytes(data)
        return path

    def test_inventory_extraction_and_boot_lookup(self):
        with ISO9660(self.write_iso(make_iso())) as disc:
            self.assertEqual(len(disc.entries), 4)
            entry = disc.lookup("slus_000.00;1")
            self.assertEqual(entry.offset, 22 * 2048)
            self.assertEqual(parse_boot2(disc.read(disc.lookup("SYSTEM.CNF"))), "SLUS_000.00")
            artifact = disc.extract(entry, self.root / "extract")
            self.assertEqual(artifact.read_bytes(), make_elf())

    def test_cli_run_magic_probes_without_asset_dump(self):
        iso = self.write_iso(make_iso())
        out = self.root / "out"
        result = run(iso, Path(os.path.relpath(out)), hash_iso=True)
        self.assertEqual(len(result["binaries"]), 2)
        self.assertEqual(result["file_count"], 4)
        self.assertIsNotNone(result["iso_sha256"])
        self.assertTrue((out / "disc" / "RAWBIN").is_file())
        self.assertFalse((out / "disc" / "ASSET.DAT").exists())
        self.assertTrue((out / "disc_inventory.json").is_file())

    def test_explicit_full_disc_extraction_includes_assets(self):
        result = run(self.write_iso(make_iso()), self.root / "out", extract_all_disc=True)
        self.assertEqual(result["disc_extraction"], "all files")
        self.assertEqual((self.root / "out" / "disc" / "ASSET.DAT").read_bytes(), b"plainasset")

    def test_dual_endian_disagreement_and_truncation(self):
        with self.assertRaises(DiscError):
            both16(b"\x01\x00\x00\x02", 0)
        with self.assertRaises(DiscError):
            both32(bytes(7), 0)
        data = make_iso()
        data[16 * 2048 + 83] = 1
        with self.assertRaisesRegex(DiscError, "dual-endian"):
            ISO9660(self.write_iso(data))

    def test_volume_and_file_extent_bounds(self):
        data = make_iso()
        data[16 * 2048 + 80:16 * 2048 + 88] = struct.pack("<I", 99) + struct.pack(">I", 99)
        with self.assertRaisesRegex(DiscError, "beyond image"):
            ISO9660(self.write_iso(data))
        data = make_iso([record(b"BAD.BIN;1", 31, 4096)])
        with self.assertRaisesRegex(DiscError, "outside volume"):
            ISO9660(self.write_iso(data))

    def test_multi_extent_and_interleaved_fail_explicitly(self):
        data = make_iso([record(b"MULTI.BIN;1", 25, 10, 0x80)])
        with self.assertRaisesRegex(UnsupportedDiscError, "Multi-extent"):
            ISO9660(self.write_iso(data))
        extra = record(b"INTER.BIN;1", 25, 10)
        extra[26] = 1
        with self.assertRaisesRegex(UnsupportedDiscError, "Interleaved"):
            ISO9660(self.write_iso(make_iso([extra])))

    def test_directory_cycle_and_duplicate_names_rejected(self):
        with self.assertRaisesRegex(DiscError, "cycle"):
            ISO9660(self.write_iso(make_iso([record(b"LOOP", 20, 2048, 2)])))
        with self.assertRaisesRegex(DiscError, "duplicate"):
            ISO9660(self.write_iso(make_iso([record(b"ASSET.DAT;2", 25, 10)])))

    def test_malformed_directory_record(self):
        data = make_iso()
        data[20 * 2048] = 33
        with self.assertRaises(DiscError):
            ISO9660(self.write_iso(data))

    def test_extraction_path_traversal_rejected(self):
        for path in ("../outside", "a/../../outside", "C:/outside", "/outside", "a\\outside", "a//b", "a/./b",
                     "CON", "a/NUL.txt", "a/file.", "a/file ", "a/file?", "a/line\nbreak"):
            with self.subTest(path=path), self.assertRaises(DiscError):
                safe_destination(self.root, path)
        for identifier in (b"..;1", b"../OUT;1", b"C:OUT;1", b"..\\OUT;1"):
            with self.subTest(identifier=identifier), self.assertRaises(DiscError):
                clean_identifier(identifier)
        with self.assertRaises(DiscError):
            ISO9660(self.write_iso(make_iso([record(b"../OUT;1", 25, 10)])))
        with self.assertRaises(DiscError):
            parse_boot2(b"BOOT2=cdrom0:\\..\\OUT;1")

    def test_symlink_escape_rejected_when_available(self):
        root = self.root / "root"
        root.mkdir()
        link = root / "link"
        try:
            link.symlink_to(self.root, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("This host does not allow creating test symlinks")
        with self.assertRaises(DiscError):
            safe_destination(root, "link/escape")


if __name__ == "__main__":
    unittest.main()
