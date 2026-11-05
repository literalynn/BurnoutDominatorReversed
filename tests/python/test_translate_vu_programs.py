"""Control flow, branch backup and XGKICK waits of translate_vu_programs.py on synthetic VU1 code; no game bytes.

The C++ it writes is checked against the microcode on recorded starts by vu1_native_tests."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import struct
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "translate_vu_programs.py"
spec = importlib.util.spec_from_file_location("translate_vu_programs", TOOL)
tvp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tvp)

NOP_LOWER, NOP_UPPER, E_BIT = 0x8000033C, 0x000002FF, 0x40000000
ADD = 0x01E10868         # ADD.xyzw vf1, vf1, vf1
XGKICK = 0x80000EFC      # XGKICK vi1
IADDIU_3 = 0x10010003    # IADDIU vi1, vi0, 3
ISUBIU_1 = 0x12010801    # ISUBIU vi1, vi1, 1
IBNE_TO_08 = 0x52000FFE  # IBNE vi0, vi1, back to 0x08 from 0x10
B_NEXT = 0x40000000      # B to the next pair
BAL_TO_20 = 0x420F0003   # BAL vi15, 0x20 from 0x00
JR_VI15 = 0x48007800     # JR vi15


def program(*pairs):
    """(lower, upper) pairs from address 0, then NOPs up to 16 KiB."""
    code = bytearray(16384)
    for index in range(2048):
        lower, upper = pairs[index] if index < len(pairs) else (NOP_LOWER, NOP_UPPER)
        struct.pack_into("<II", code, index * 8, lower, upper)
    return bytes(code)


def translate(code):
    translator = tvp.Translator(code, 0, "test")
    translator.translate()
    return translator, "\n".join(translator.function())


END = (NOP_LOWER, NOP_UPPER | E_BIT)
NOP = (NOP_LOWER, NOP_UPPER)


class Translate(unittest.TestCase):
    def test_branch_right_after_an_integer_write_reads_the_old_value(self):
        translator, text = translate(program((IADDIU_3, NOP_UPPER), (ISUBIU_1, NOP_UPPER), (IBNE_TO_08, NOP_UPPER),
                                             NOP, END, NOP))
        self.assertEqual(translator.footprint(), (0, 0x30))
        self.assertIn("vi1 = vi16(vi1 - 1);", text)
        self.assertIn("viOld = vi1;", text)
        self.assertIn("taken = static_cast<int16_t>(viOld) != static_cast<int16_t>(0);", text)
        self.assertIn("endPc = 0x0030u;", text)

    def test_branch_in_a_delay_slot_is_refused(self):
        with self.assertRaisesRegex(tvp.TranslationError, "delay slot"):
            translate(program((B_NEXT, NOP_UPPER), (B_NEXT, NOP_UPPER), END, NOP))

    def test_jr_without_a_link_is_refused(self):
        with self.assertRaisesRegex(tvp.TranslationError, "JR vi15 without a link"):
            translate(program((JR_VI15, NOP_UPPER), NOP, END, NOP))

    def test_jr_returns_to_the_bal_links(self):
        _, text = translate(program((BAL_TO_20, NOP_UPPER), NOP, END, NOP, (JR_VI15, NOP_UPPER), NOP))
        self.assertIn("vi15 = 2;", text)
        self.assertRegex(text, r"case 0x0010u: goto L0010_\d+;")
        self.assertIn("default: unexpectedJump(c, jump); return;", text)

    def test_xgkick_waits_are_resolved_at_run_time(self):
        translator, text = translate(program((XGKICK, ADD), (XGKICK, NOP_UPPER), END, NOP))
        # The ADD's MAC flags land 3 cycles after the second kick could issue: one path per wait.
        self.assertIn("switch (wait < 3u ? wait : 3u) {", text)
        self.assertEqual(text.count("kickEnd = xgkick(c, vi1, cycle);"), 1 + 4)
        self.assertEqual(translator.needed, {0})  # the end of the program reads the flags


if __name__ == "__main__":
    unittest.main()
