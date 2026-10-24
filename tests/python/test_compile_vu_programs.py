"""FMAC flag hints of compile_vu_programs.py on a synthetic VU1 loop; no game bytes."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "compile_vu_programs.py"
spec = importlib.util.spec_from_file_location("compile_vu_programs", TOOL)
cvp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cvp)

NOP_LOWER, NOP_UPPER, E_BIT = 0x8000033C, 0x000002FF, 0x40000000
ADD = 0x01E10868        # ADD.xyzw vf1, vf1, vf1
IBNE_BACK = 0x52000FFC  # IBNE vi0, vi1, -4
FMAND = 0x34021800      # FMAND vi2, vi3


def loop(after_loop_lower):
    """0-4: a loop whose FMAC at 2 sits in the last 4 pairs of its block;
    5-11: an FMAC, the given lower at 6, then the end."""
    words = [(NOP_LOWER, NOP_UPPER)] * cvp.PAIRS
    words[0] = (NOP_LOWER, ADD)
    words[2] = (NOP_LOWER, ADD)
    words[3] = (IBNE_BACK, NOP_UPPER)
    words[5] = (NOP_LOWER, ADD)
    words[6] = (after_loop_lower, NOP_UPPER)
    words[10] = (NOP_LOWER, NOP_UPPER | E_BIT)
    return cvp.Routine([cvp.Pair(lower, upper) for lower, upper in words], 0)


class FlagHints(unittest.TestCase):
    def test_loop_back_and_fall_through_hide_the_last_result(self):
        routine = loop(NOP_LOWER)
        self.assertEqual(sorted(routine.blocks), [0, 5])
        hidden = cvp.STATUS_UNSEEN | cvp.FLAGS_UNSEEN
        self.assertEqual(cvp.flag_hints(routine, 0, True), {0: hidden, 2: hidden})
        # Nothing hides the last result before the end.
        self.assertEqual(cvp.flag_hints(routine, 5, True), {0: cvp.STATUS_UNSEEN})
        # With the status seen, whatever follows the block reads.
        self.assertEqual(cvp.flag_hints(routine, 0, False), {})

    def test_reader_after_the_loop_sees_the_last_result(self):
        routine = loop(FMAND)
        # FMAND at 6 is 4 pairs after the FMAC at 2, 1 after the one at 5.
        self.assertEqual(cvp.flag_hints(routine, 0, True),
                         {0: cvp.STATUS_UNSEEN | cvp.FLAGS_UNSEEN, 2: cvp.STATUS_UNSEEN})


if __name__ == "__main__":
    unittest.main()
