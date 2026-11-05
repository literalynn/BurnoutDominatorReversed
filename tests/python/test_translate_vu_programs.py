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
JR_VI12 = 0x48006000     # JR vi12
IADDIU_VI15_4 = 0x100F0004  # IADDIU vi15, vi0, 4
IADDIU_VI13_5 = 0x100D0005  # IADDIU vi13, vi0, 5
ILW_Y_VI12 = 0x088C03F5  # ILW.y vi12, 1013(vi0)
ISW_Y_VI13 = 0x0A8D03F5  # ISW.y vi13, 1013(vi0)
XTOP_VI1 = 0x800106BC    # XTOP vi1
ILW_W_VI5 = 0x082503F5   # ILW.w vi5, 1013(vi0)
IBEQ_VI5_TO_30 = 0x50002803  # IBEQ vi5, vi0, 0x30 from 0x10
IADDIU_VI13_9 = 0x100D0009  # IADDIU vi13, vi0, 9
SQRT_VF1X = 0x800103BD   # SQRT Q, vf1x
RSQRT_VF1X = 0x80010BBE  # RSQRT Q, vf1x, vf1x
WAITQ = 0x800003BF
ESQRT_VF1X = 0x80000FBC  # ESQRT P, vf1x
WAITP = 0x800007BF
MFP_VF2 = 0x81E2067C     # MFP.xyzw vf2, P


def program(*pairs):
    """(lower, upper) pairs from address 0, then NOPs up to 16 KiB."""
    code = bytearray(16384)
    for index in range(2048):
        lower, upper = pairs[index] if index < len(pairs) else (NOP_LOWER, NOP_UPPER)
        struct.pack_into("<II", code, index * 8, lower, upper)
    return bytes(code)


def translate(code, entry=0):
    translator = tvp.Translator(code, entry, "test")
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

    def test_jr_to_an_address_nothing_sets_is_refused(self):
        with self.assertRaisesRegex(tvp.TranslationError, "no known value"):
            translate(program((JR_VI15, NOP_UPPER), NOP, END, NOP))

    def test_jr_returns_to_the_bal_links(self):
        _, text = translate(program((BAL_TO_20, NOP_UPPER), NOP, END, NOP, (JR_VI15, NOP_UPPER), NOP))
        self.assertIn("vi15 = 2;", text)
        self.assertRegex(text, r"case 0x0010u: goto L0010_\d+;")
        self.assertIn("default: unexpectedJump(c, jump); return true;", text)
        self.assertNotIn("return false;", text)

    def test_entry_stub_resumes_where_the_last_run_said(self):
        # The run before ended at 0x00 with vi15 = 4; the game continues at 0x10, JR vi15.
        translator, text = translate(program((IADDIU_VI15_4, NOP_UPPER | E_BIT), NOP, (JR_VI15, NOP_UPPER), NOP,
                                             (XTOP_VI1, NOP_UPPER), (IADDIU_VI15_4, NOP_UPPER | E_BIT), NOP), entry=0x10)
        self.assertEqual(translator.footprint(), (0x10, 0x38))
        self.assertIn("    if (vi15 != 4)\n        return false;", text)
        self.assertRegex(text, r"case 0x0020u: goto L0020_\d+;")
        self.assertIn("vi1 = static_cast<int32_t>(s.top & 0x3FFu);", text)

    def test_jr_through_a_word_the_program_stores(self):
        # The variant address the run before stored in VU1 memory; this run stores the next one.
        translator, text = translate(program((ILW_Y_VI12, NOP_UPPER), (IADDIU_VI13_5, NOP_UPPER), (ISW_Y_VI13, NOP_UPPER),
                                             (JR_VI12, NOP_UPPER), NOP, END, NOP))
        self.assertEqual(translator.jr_targets, {0x18: {0x28}})
        self.assertIn("    if (ilw(mem, 1013, 1) != 5)\n        return false;", text)

    def test_xgkick_waits_are_resolved_at_run_time(self):
        translator, text = translate(program((XGKICK, ADD), (XGKICK, NOP_UPPER), END, NOP))
        # The ADD's MAC flags land 3 cycles after the second kick could issue: one path per wait.
        self.assertIn("switch (wait < 3u ? wait : 3u) {", text)
        self.assertEqual(text.count("kickEnd = xgkick(c, vi1, cycle);"), 1 + 4)
        self.assertEqual(translator.needed, {0})  # the end of the program reads the flags


    def test_start_value_checked_only_on_the_path_that_reads_it(self):
        # A flag in 1013.w asks for a new variant, stored in 1013.y; without it, the last one.
        translator, text = translate(program((ILW_W_VI5, NOP_UPPER), NOP, (IBEQ_VI5_TO_30, NOP_UPPER), NOP,
                                             (IADDIU_VI13_9, NOP_UPPER), (ISW_Y_VI13, NOP_UPPER),
                                             (ILW_Y_VI12, NOP_UPPER), (JR_VI12, NOP_UPPER), NOP, END, NOP))
        self.assertEqual(translator.jr_targets, {0x38: {0x48}})
        self.assertIn("    if (ilw(mem, 1013, 3) == 0 && ilw(mem, 1013, 1) != 9)\n        return false;", text)


    def test_sqrt_and_rsqrt_land_in_q_after_their_latency(self):
        for op, call, cycles in ((SQRT_VF1X, "qp = sqrtQ(lane<0>(f1));", 10),
                                 (RSQRT_VF1X, "qp = rsqrtQ(lane<0>(f1), lane<0>(f1));", 16)):
            _, text = translate(program((op, NOP_UPPER), (WAITQ, NOP_UPPER), END, NOP))
            self.assertIn(call, text)
            self.assertIn("q = qp;", text)
            self.assertIn(f"cycle += {cycles};", text)  # WAITQ issues when Q lands, at 7 or 13

    def test_efu_result_reaches_p_then_mfp(self):
        translator, text = translate(program((ESQRT_VF1X, NOP_UPPER), (WAITP, NOP_UPPER), (MFP_VF2, NOP_UPPER), END, NOP))
        self.assertIn("p_0000 = esqrt(lane<0>(f1));", text)
        self.assertLess(text.index("p = p_0000;"), text.index("const V lv = splat(p);"))
        self.assertIn("s.p = p;", text)
        # WAITP issues at 12, MFP at 13; the end waits for MFP's vf2 at 17, not for the EFU.
        self.assertIn("cycle += 17;", text)


if __name__ == "__main__":
    unittest.main()
