#!/usr/bin/env python3
"""Add code entry points that Ghidra's function map misses.

Ghidra only creates functions it reaches by flow analysis. Code that is only
reached through a pointer (C++ virtual methods, callbacks, thread entries,
jump tables) is left as undefined bytes, so the recompiler never translates it
and the runtime stops on the first indirect call into it.

Candidates, all limited to 4-aligned addresses inside .text:
  * 32-bit words in the data sections whose value points into .text;
  * lui + addiu/ori pairs in .text that build a .text address;
  * starts of undefined code in the gaps between Ghidra functions (first
    non-zero word of a gap, and the first one after each jr/j + delay slot).

A candidate is kept when its first instructions use valid R5900 opcodes. Rows
for candidates inside an existing function end with that function; rows in a
gap end at the next known function. Translating a few data words as code is
harmless (it is never called); a missing entry point stops the game.

Usage: augment_function_map.py <ELF> <map.csv> <analysis.json> <out.csv> <report.json>
"""
from __future__ import annotations

import bisect
import csv
import json
import struct
import sys
from pathlib import Path

DATA_SECTIONS = (".data", ".rodata", ".sdata", ".lit4", ".gcc_except_table")
# R5900 primary opcodes without an instruction (MIPS III/IV ones the EE lacks).
INVALID_OPCODES = {0x13, 0x1D, 0x30, 0x32, 0x34, 0x35, 0x38, 0x3A, 0x3B, 0x3C, 0x3D}
VALIDATE_WORDS = 4


class Elf:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        phoff, = struct.unpack_from("<I", self.data, 0x1C)
        phnum, = struct.unpack_from("<H", self.data, 0x2C)
        self.segments = []
        for i in range(phnum):
            kind, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", self.data, phoff + i * 32)
            if kind == 1:
                self.segments.append((vaddr, offset, filesz))

    def word(self, address: int) -> int | None:
        for vaddr, offset, size in self.segments:
            if vaddr <= address < vaddr + size - 3:
                return struct.unpack_from("<I", self.data, offset + address - vaddr)[0]
        return None


def is_return_or_jump(word: int) -> bool:
    op = word >> 26
    if op == 0x02:  # j
        return True
    return op == 0 and (word & 0x3F) == 0x08  # jr


def plausible_code(elf: Elf, address: int, end: int) -> bool:
    for offset in range(VALIDATE_WORDS):
        a = address + offset * 4
        if a >= end:
            break
        word = elf.word(a)
        if word is None or (word >> 26) in INVALID_OPCODES:
            return False
        if offset == 0 and word == 0:
            return False
    return True


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__)
        return 2
    elf_path, map_path, analysis_path, out_path, report_path = map(Path, sys.argv[1:])
    elf = Elf(elf_path)
    sections = {s["name"]: s for s in json.loads(analysis_path.read_text(encoding="utf-8"))["sections"]}
    text = sections[".text"]
    text_start, text_end = text["address"], text["address"] + text["size"]

    with map_path.open(encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    known = {int(r["Start"], 16) for r in rows}
    # Top-level functions only (labels share their parent's end).
    functions = sorted({(int(r["Start"], 16), int(r["End"], 16)) for r in rows
                        if not r["Name"].startswith(("entry_", "LAB_"))})
    starts = [s for s, _ in functions]

    def containing(address: int):
        i = bisect.bisect_right(starts, address) - 1
        while i >= 0:
            start, end = functions[i]
            if start <= address < end:
                return start, end
            if end <= address:
                return None
            i -= 1
        return None

    def next_function(address: int) -> int:
        i = bisect.bisect_right(starts, address)
        return starts[i] if i < len(starts) else text_end

    reasons: dict[int, set[str]] = {}

    def add(address: int, reason: str) -> None:
        if text_start <= address < text_end and address % 4 == 0 and address not in known:
            reasons.setdefault(address, set()).add(reason)

    for name in DATA_SECTIONS:
        section = sections.get(name)
        if not section:
            continue
        begin = section["address"] & ~3
        for address in range(begin, section["address"] + section["size"], 4):
            word = elf.word(address)
            if word is not None:
                add(word, "pointer:" + name)

    last_lui: dict[int, tuple[int, int]] = {}
    for address in range(text_start, text_end, 4):
        word = elf.word(address)
        op, rs, rt, imm = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 0xFFFF
        if op == 0x0F:
            last_lui[rt] = (imm << 16, address)
        elif op in (0x09, 0x19, 0x0D) and rs in last_lui and address - last_lui[rs][1] <= 64:
            low = imm - 0x10000 if op != 0x0D and imm & 0x8000 else imm
            add((last_lui[rs][0] + low) & 0xFFFFFFFF, "code-constant")

    cursor = text_start
    for start, end in functions + [(text_end, text_end)]:
        if start > cursor:
            expect_start = True
            address = cursor
            while address < start:
                word = elf.word(address)
                if word and expect_start:
                    add(address, "gap-code")
                    expect_start = False
                if word and is_return_or_jump(word):
                    address += 8  # delay slot
                    expect_start = True
                    continue
                address += 4
        cursor = max(cursor, end)

    added = []
    rejected = []
    for address in sorted(reasons):
        parent = containing(address)
        end = parent[1] if parent else next_function(address)
        why = sorted(reasons[address])
        if not plausible_code(elf, address, end):
            rejected.append({"address": f"0x{address:08X}", "reasons": why})
            continue
        added.append({"address": address, "end": end, "inside": parent is not None, "reasons": why})

    out_rows = rows + [{"Name": f"ptr_{a['address']:08x}", "Start": f"0x{a['address']:08X}",
                        "End": f"0x{a['end']:08X}", "Size": str(a["end"] - a["address"])}
                       for a in added]
    out_rows.sort(key=lambda r: (int(r["Start"], 16), r["Name"]))
    with out_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["Name", "Start", "End", "Size"], lineterminator="\n")
        writer.writeheader()
        writer.writerows(out_rows)

    report = {
        "source_map": str(map_path),
        "source_rows": len(rows),
        "added": len(added),
        "added_in_gaps": sum(not a["inside"] for a in added),
        "added_inside_functions": sum(a["inside"] for a in added),
        "rejected_invalid_opcodes": len(rejected),
        "entries": [{"address": f"0x{a['address']:08X}", "end": f"0x{a['end']:08X}",
                     "inside_function": a["inside"], "reasons": a["reasons"]} for a in added],
        "rejected": rejected,
    }
    report_path.write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    print(f"{len(rows)} rows + {len(added)} entry points "
          f"({report['added_in_gaps']} in gaps, {report['added_inside_functions']} inside functions); "
          f"{len(rejected)} rejected -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
