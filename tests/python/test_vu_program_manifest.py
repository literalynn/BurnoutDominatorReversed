"""VU program manifest round trip on a synthetic program; no game bytes."""
from __future__ import annotations

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[2] / "tools" / "vu_program_manifest.py"
NOP_LOWER, NOP_UPPER, E_BIT = 0x8000033C, 0x000002FF, 0x40000000


class ManifestRoundTrip(unittest.TestCase):
    def test_vu0_image_comes_back_as_vu0(self):
        # NOP with the E bit, then the NOP in its delay slot.
        code = struct.pack("<4I", NOP_LOWER, NOP_UPPER | E_BIT, NOP_LOWER, NOP_UPPER)
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            recording, game, expanded = temp / "rec", temp / "game", temp / "out"
            recording.mkdir()
            game.mkdir()
            (recording / "vu0-a.code").write_bytes(code + bytes(4096 - len(code)))
            (recording / "entries.txt").write_text("a 0\n")
            (game / "GAME.BIN").write_bytes(bytes(range(256)) * 4 + code + bytes(64))
            manifest = temp / "vu.manifest"
            run = lambda *args: subprocess.run([sys.executable, str(TOOL), *map(str, args)], check=True,
                                               capture_output=True)
            run("make", "--output", manifest, "--game-root", game, "--game", game / "GAME.BIN", recording)
            self.assertIn(" vu0\n", manifest.read_text())
            run("expand", "--manifest", manifest, "--game-root", game, "--output", expanded)
            images = list(expanded.glob("*.code"))
            self.assertEqual([image.name[:4] for image in images], ["vu0-"])
            self.assertEqual(len(images[0].read_bytes()), 4096)
            self.assertEqual(images[0].read_bytes()[:len(code)], code)
            self.assertEqual((expanded / "entries.txt").read_text().split(), [images[0].stem[4:], "0"])


if __name__ == "__main__":
    unittest.main()
