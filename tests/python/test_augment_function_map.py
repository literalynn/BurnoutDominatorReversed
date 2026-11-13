"""augment_function_map.py on a synthetic ELF: entry points inside a function are routed to it, not
translated again as rows; no game bytes."""
from __future__ import annotations

import csv
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "augment_function_map.py"
TEXT, DATA = 0x100000, 0x100060
ADDIU = 0x24020001  # addiu $v0, $zero, 1


def elf(words: list[int]) -> bytes:
    """A 32-bit ELF with one loadable segment holding the words at TEXT."""
    body = struct.pack(f"<{len(words)}I", *words)
    header = bytearray(0x34)
    header[:4] = b"\x7fELF"
    struct.pack_into("<I", header, 0x1C, 0x34)  # program headers
    struct.pack_into("<H", header, 0x2C, 1)
    segment = struct.pack("<8I", 1, 0x100, TEXT, TEXT, len(body), len(body), 5, 4)
    return bytes(header) + segment + bytes(0x100 - 0x34 - len(segment)) + body


class RoutingTest(unittest.TestCase):
    def test_entry_points_inside_a_function_are_routed_to_it(self):
        # FUN_a: 0x100000-0x100040; a gap of code up to 0x100060; .data holds a pointer into FUN_a.
        words = [ADDIU] * ((DATA - TEXT) // 4) + [0x100030]
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "game.elf").write_bytes(elf(words))
            (root / "analysis.json").write_text(json.dumps({"sections": [
                {"name": ".text", "address": TEXT, "size": DATA - TEXT},
                {"name": ".data", "address": DATA, "size": 4}]}), encoding="utf-8")
            with (root / "map.csv").open("w", encoding="utf-8", newline="") as f:
                writer = csv.writer(f, lineterminator="\n")
                writer.writerows([["Name", "Start", "End", "Size"],
                                  ["FUN_00100000", "0x00100000", "0x00100040", "64"],
                                  ["entry_00100010", "0x00100010", "0x00100020", "16"]])
            subprocess.run([sys.executable, str(TOOL), str(root / "game.elf"), str(root / "map.csv"),
                            str(root / "analysis.json"), str(root / "out.csv"), str(root / "report.json")],
                           check=True, capture_output=True)
            with (root / "out.csv").open(encoding="utf-8") as f:
                rows = [row["Name"] for row in csv.DictReader(f)]
            report = json.loads((root / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(rows, ["FUN_00100000", "ptr_00100040"])  # the label is gone, the gap's code stays a row
        self.assertEqual(report["routed"], ["0x00100010", "0x00100030"])
        self.assertEqual(report["labels_inside_functions"], 1)


if __name__ == "__main__":
    unittest.main()
