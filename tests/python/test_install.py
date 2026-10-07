"""Decision logic of tools/install.py; no game files or toolchains are used."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import unittest
from unittest import mock
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import install  # noqa: E402
import project  # noqa: E402


class InstallTests(unittest.TestCase):
    def setUp(self):
        parent = Path(__file__).resolve().parent
        self.root = parent / ("install-test-" + uuid.uuid4().hex)
        self.root.mkdir()
        self.root.resolve().relative_to(parent)
        self.addCleanup(shutil.rmtree, self.root)
        self.work = self.root / "work"
        self.local = self.work / "local"
        self.local.mkdir(parents=True)
        self.elf = b"\x7fELF" + bytes(60)
        self.lock = {"boot_path": "SLES_546.27", "elf_size": len(self.elf),
                     "elf_sha256": hashlib.sha256(self.elf).hexdigest()}

    def test_work_dir_priority(self):
        checkout = self.root / "checkout"
        checkout.mkdir()
        home = self.root / "home"
        for windows in (False, True):
            default = Path("D:\\", "bdr-work") if windows else (home / "bdr-work").resolve()
            # os.environ copies have upper-case keys on Windows.
            self.assertEqual(install.choose_work_dir(None, {"SYSTEMDRIVE": "D:"}, checkout, home, windows), (default, True))
        self.assertEqual(install.choose_work_dir(None, {}, checkout, home, True), (Path("C:\\", "bdr-work"), True))
        (checkout / "work.json").write_text(json.dumps({"work_dir": str(self.root / "configured")}), encoding="utf-8")
        self.assertEqual(install.choose_work_dir(None, {}, checkout, home, False), ((self.root / "configured").resolve(), False))
        self.assertEqual(install.choose_work_dir(self.root / "asked", {}, checkout, home, False), ((self.root / "asked").resolve(), True))
        environ = {"BDR_WORK_DIR": str(self.root / "env")}
        self.assertEqual(install.choose_work_dir(None, environ, checkout, home, False), ((self.root / "env").resolve(), False))
        with self.assertRaises(install.SetupError):
            install.choose_work_dir(self.root / "asked", environ, checkout, home, False)

    def test_work_dir_must_be_ascii_on_windows(self):
        accented = self.root / "Hélène" / "bdr-work"
        self.assertEqual(install.choose_work_dir(accented, {}, self.root, self.root, False), (accented.resolve(), True))
        with self.assertRaises(install.SetupError):
            install.choose_work_dir(accented, {}, self.root, self.root, True)
        with self.assertRaises(install.SetupError):
            install.choose_work_dir(None, {"BDR_WORK_DIR": str(accented)}, self.root, self.root, True)

    def test_jobs_default(self):
        self.assertEqual(install.default_jobs(None), 4)
        self.assertEqual(install.default_jobs(12), 12)
        self.assertEqual(install.default_jobs(128), 64)

    def test_required_space(self):
        everything = install.EXTRACTION_BYTES + install.TRANSLATION_BYTES + install.BUILD_BYTES
        self.assertEqual(install.required_space(disc_ready=False, game_built=False), everything)
        self.assertEqual(install.required_space(disc_ready=True, game_built=False),
                         install.TRANSLATION_BYTES + install.BUILD_BYTES)
        self.assertEqual(install.required_space(disc_ready=True, game_built=True), install.TRANSLATION_BYTES)

    def test_disc_state(self):
        self.assertEqual(install.disc_state(self.local, self.lock), "missing")
        (self.local / "disc").mkdir()
        (self.local / "disc" / "SLES_546.27").write_bytes(self.elf)
        self.assertEqual(install.disc_state(self.local, self.lock), "partial")
        inventory = self.local / "disc_inventory.json"
        inventory.write_text(json.dumps({"disc_extraction": "selected executables and SYSTEM.CNF"}), encoding="utf-8")
        self.assertEqual(install.disc_state(self.local, self.lock), "partial")
        inventory.write_text(json.dumps({"disc_extraction": "all files"}), encoding="utf-8")
        self.assertEqual(install.disc_state(self.local, self.lock), "ok")
        (self.local / "disc" / "SLES_546.27").write_bytes(self.elf[:-1] + b"\x01")
        self.assertEqual(install.disc_state(self.local, self.lock), "missing")

    def test_iso_choice(self):
        iso = self.root / "game.iso"
        moved = self.root / "moved.iso"
        with self.assertRaises(install.SetupError):
            install.choose_iso(None, None, False)
        with self.assertRaises(install.SetupError):
            install.choose_iso(None, moved, True)  # recorded ISO no longer there
        with self.assertRaises(install.SetupError):
            install.choose_iso(moved, None, False)
        iso.write_bytes(b"disc")
        self.assertEqual(install.choose_iso(None, iso, False), iso.resolve())
        moved.write_bytes(b"disc")
        self.assertEqual(install.choose_iso(moved, iso, False), moved.resolve())

    def test_iso_verification_is_recorded_for_later_runs(self):
        iso = self.root / "game.iso"
        iso.write_bytes(b"disc")
        lock = {**self.lock, "iso_size": 4, "iso_sha256": hashlib.sha256(b"disc").hexdigest()}
        quiet = contextlib.redirect_stdout(io.StringIO())
        with quiet:
            self.assertTrue(install.verify_iso(iso, self.local, lock))
        no_hash = mock.patch.object(install, "sha256", side_effect=AssertionError("hashed again"))
        with no_hash:
            self.assertFalse(install.verify_iso(iso, self.local, lock))
        # project.py run accepts the same record without hashing the ISO again.
        (self.local / "paths.json").write_text(json.dumps({"iso": str(iso)}), encoding="utf-8")
        with mock.patch.object(project, "LOCAL", self.local), mock.patch.object(project, "LOCK", lock), \
                mock.patch.object(project, "sha256", side_effect=AssertionError("hashed again")):
            self.assertEqual(project.verified_iso(), iso)
        # A modified file is checked again.
        stat = iso.stat()
        os.utime(iso, ns=(stat.st_atime_ns, stat.st_mtime_ns + 2_000_000_000))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(install.verify_iso(iso, self.local, lock))
        iso.write_bytes(b"DISC")
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(install.SetupError):
            install.verify_iso(iso, self.local, lock)
        iso.write_bytes(b"disc!")
        with self.assertRaises(install.SetupError):
            install.verify_iso(iso, self.local, lock)

    def test_project_run_points_to_the_installer_without_an_iso(self):
        with mock.patch.object(project, "LOCAL", self.local):
            with self.assertRaisesRegex(RuntimeError, "Installer.bat"):
                project.verified_iso()
            (self.local / "paths.json").write_text(json.dumps({"iso": str(self.root / "moved.iso")}), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "install.py"):
                project.verified_iso()

    def test_shipped_map_is_used_in_place(self):
        shipped = self.root / "data" / "functions.ee.csv"
        shipped.parent.mkdir()
        shipped.write_text("Name,Start,End,Size\nentry,0x00100008,0x00100218,528\n", encoding="utf-8")
        self.assertEqual(install.choose_function_map(self.local, None, shipped), shipped)
        self.assertFalse((self.local / "analysis").exists())
        # A map kept in the work directory wins over the shipped one, --function-map over both.
        work_map = self.local / "analysis" / "ghidra" / "functions.ee.csv"
        work_map.parent.mkdir(parents=True)
        work_map.write_text("Name,Start,End,Size\n", encoding="utf-8")
        self.assertEqual(install.choose_function_map(self.local, None, shipped), work_map)
        self.assertEqual(install.choose_function_map(self.local, shipped, shipped), shipped.resolve())
        with self.assertRaises(install.SetupError):
            install.choose_function_map(self.local, self.root / "absent.csv", shipped)
        with self.assertRaises(install.SetupError):
            install.choose_function_map(self.root / "empty", None, self.root / "absent.csv")

    def test_generation_changes(self):
        generated = self.work / "generated"
        generated.mkdir()
        current = {"elf_sha256": self.lock["elf_sha256"], "ps2recomp_commit": "c1", "tool_sha256": "t1",
                   "augmenter_sha256": "a1", "function_map_sha256": "m1"}
        self.assertEqual(install.generation_changes(generated, self.local, current), ["translation"])
        record = generated / "generation.json"
        written = {"elf_sha256": self.lock["elf_sha256"], "ps2recomp_commit": "c1", "tool_sha256": "t1",
                   "augmenter_sha256": "a1", "function_map_sha256": "augmented map", "augmentation": None}
        record.write_text(json.dumps(written), encoding="utf-8")
        self.assertEqual(install.generation_changes(generated, self.local, current), ["augmentation"])
        record.write_text(json.dumps({**written, "augmentation": {"added": 1}}), encoding="utf-8")
        install.write_state(self.local, current)
        self.assertEqual(install.generation_changes(generated, self.local, current), [])
        # Each input triggers a new translation: generation.json is authoritative for
        # the values project.py records, install-state.json holds the source map.
        for key in current:
            changed = {**current, key: "new"}
            self.assertEqual(install.generation_changes(generated, self.local, changed), [key], key)

    def test_legacy_translation_is_kept_once(self):
        generated = self.work / "generated"
        generated.mkdir()
        current = {"elf_sha256": self.lock["elf_sha256"], "ps2recomp_commit": "c1", "tool_sha256": "t1",
                   "augmenter_sha256": "a1", "function_map_sha256": "m1"}
        # Written before augmenter_sha256 existed, and before the installer.
        legacy = {"elf_sha256": self.lock["elf_sha256"], "ps2recomp_commit": "c1", "tool_sha256": "t1",
                  "augmentation": {"added": 1}}
        (generated / "generation.json").write_text(json.dumps(legacy), encoding="utf-8")
        self.assertEqual(install.generation_changes(generated, self.local, current), [])
        state = json.loads((self.local / install.STATE_NAME).read_text(encoding="utf-8"))
        self.assertEqual(state, current)
        self.assertEqual(install.generation_changes(generated, self.local, current), [])
        self.assertEqual(install.generation_changes(generated, self.local, {**current, "augmenter_sha256": "a2"}),
                         ["augmenter_sha256"])
        self.assertEqual(install.generation_changes(generated, self.local, {**current, "function_map_sha256": "m2"}),
                         ["function_map_sha256"])
        # A recorded value is never backfilled: a rebuilt recompiler retranslates.
        self.assertEqual(install.generation_changes(generated, self.local, {**current, "tool_sha256": "t2"}),
                         ["tool_sha256"])
        # State from the first installer: only the map was remembered.
        install.write_state(self.local, {"function_map_sha256": "m1"})
        self.assertEqual(install.generation_changes(generated, self.local, {**current, "augmenter_sha256": "a2"}), [])
        self.assertEqual(json.loads((self.local / install.STATE_NAME).read_text(encoding="utf-8"))["augmenter_sha256"], "a2")
        install.write_state(self.local, {"function_map_sha256": "m0"})
        self.assertEqual(install.generation_changes(generated, self.local, current), ["function_map_sha256"])

    def test_backups_and_interrupted_translation_cleanup(self):
        backups = self.local / "backups"
        self.assertEqual(install.prune_backups(backups, self.work), [])
        names = ["generated-20261001-090000-000001", "generated-20261003-090000-000001",
                 "generated-20261002-090000-000001", "generated-manual-copy"]
        for name in names:
            (backups / name).mkdir(parents=True)
            (backups / name / "a.cpp").write_text("", encoding="utf-8")
        removed = install.prune_backups(backups, self.work)
        self.assertEqual([p.name for p in removed], [names[0], names[2]])
        self.assertEqual(sorted(p.name for p in backups.iterdir()), [names[1], names[3]])
        generated = self.work / "generated"
        generated.mkdir()
        (generated / "a.cpp").write_text("", encoding="utf-8")
        install.remove_inside(generated, self.work)
        self.assertFalse(generated.exists())
        outside = self.root / "outside"
        outside.mkdir()
        for path in (outside, self.work, self.work / "missing"):
            with self.assertRaises(install.SetupError):
                install.remove_inside(path, self.work)
        self.assertTrue(outside.is_dir() and self.work.is_dir())

    def test_build_tree_helpers(self):
        build = self.root / "build"
        self.assertIsNone(install.cache_entry(build, "CMAKE_GENERATOR"))
        self.assertIsNone(install.cached_cmake(build))
        self.assertIsNone(install.find_recompiler(build))
        self.assertIsNone(install.find_game(build))
        build.mkdir()
        cmake = self.root / "cmake-bin" / "cmake"
        (build / "CMakeCache.txt").write_text(
            "// The CMake executable\nCMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n"
            f"CMAKE_COMMAND:INTERNAL={cmake.as_posix()}\n", encoding="utf-8")
        self.assertEqual(install.cache_entry(build, "CMAKE_GENERATOR"), "Visual Studio 18 2026")
        self.assertIsNone(install.cached_cmake(build))  # no longer installed
        cmake.parent.mkdir()
        cmake.write_bytes(b"")
        self.assertEqual(install.cached_cmake(build), cmake.as_posix())
        tool = build / "ps2xRecomp" / "Release" / "ps2_recomp.exe"
        tool.parent.mkdir(parents=True)
        tool.write_bytes(b"")
        self.assertEqual(install.find_recompiler(build), tool)
        game = build / "ps2xRuntime" / "burnout_dominator"
        game.parent.mkdir(parents=True)
        game.write_bytes(b"")
        self.assertEqual(install.find_game(build), game)

    def test_final_message_matches_the_platform(self):
        self.assertIn("Jouer.bat", "\n".join(install.final_message(True)))
        linux = "\n".join(install.final_message(False))
        self.assertNotIn("Jouer.bat", linux)
        self.assertIn("python3 tools/project.py run", linux)

    def test_errors_are_reported_without_traceback(self):
        failures = [install.SetupError("x"), OSError(28, "No space left on device"), KeyError("work_dir"),
                    json.JSONDecodeError("Expecting value", "", 0), ValueError("x")]
        for failure in failures:
            stderr = io.StringIO()
            with mock.patch.object(install, "setup", side_effect=failure), contextlib.redirect_stderr(stderr):
                self.assertEqual(install.main([]), 1)
            self.assertIn("ERREUR :", stderr.getvalue())

    def test_shipped_map_matches_its_documented_hash(self):
        shipped = install.SHIPPED_MAP
        self.assertTrue(shipped.is_file())
        readme = (shipped.parent / "README.md").read_text(encoding="utf-8")
        self.assertIn(hashlib.sha256(shipped.read_bytes()).hexdigest(), readme)


class ProjectRunTests(unittest.TestCase):
    def test_trace_lists_are_checked(self):
        self.assertEqual(project.hex_list("0x1E5020,1e3910"), "0x1E5020,1e3910")
        for bad in ("", "0x1E5020,", "zz"):
            with self.assertRaises(Exception):
                project.hex_list(bad)


if __name__ == "__main__":
    unittest.main()
