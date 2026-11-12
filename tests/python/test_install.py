"""Decision logic of tools/install.py; no game files or toolchains are used."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import stat
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
        self.assertEqual(install.default_jobs(16, 16 * install.GIB), 5)
        self.assertEqual(install.default_jobs(16, 2 * install.GIB), 1)
        self.assertEqual(install.default_jobs(8, 64 * install.GIB), 8)
        self.assertGreater(install.physical_memory() or 1, 0)

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
        (generated / ".git").mkdir()
        (generated / ".git" / "pack.idx").write_text("", encoding="utf-8")
        os.chmod(generated / ".git" / "pack.idx", stat.S_IREAD)  # as Git leaves its objects
        install.remove_inside(generated, self.work)
        self.assertFalse(generated.exists())
        outside = self.root / "outside"
        outside.mkdir()
        for path in (outside, self.work, self.work / "missing"):
            with self.assertRaises(install.SetupError):
                install.remove_inside(path, self.work)
        self.assertTrue(outside.is_dir() and self.work.is_dir())

    def test_purge_keeps_the_game_and_its_libraries(self):
        release = self.work / "build" / "ps2xRuntime" / "Release"
        kept = [release / "burnout_dominator.exe", release / "SDL2.dll", release / "mc0" / "card.bin", self.local / "saves" / "card.bin",
                self.local / "disc" / "SLES_546.27"]
        gone = [release / "ps2_runtime.lib", release / "burnout_dominator.dir" / "a.obj", self.work / "build" / "CMakeCache.txt",
                self.work / "build" / "ps2xRuntime" / "CMakeFiles" / "a.obj", self.work / "generated" / "a.cpp",
                self.local / "backups" / "generated-20261001-090000-000001" / "a.cpp"]
        for path in kept + gone:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x")
        install.purge(self.work)
        self.assertEqual([p for p in kept if not p.is_file()], [])
        self.assertEqual([p for p in gone if p.exists()], [])
        self.assertFalse((self.work / "generated").exists() or (self.local / "backups").exists())
        install.purge(self.work)  # nothing left to delete

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
        self.assertIn("Play.bat", "\n".join(install.final_message(True)))
        linux = "\n".join(install.final_message(False))
        self.assertNotIn("Play.bat", linux)
        self.assertIn("python3 tools/project.py run", linux)
        with mock.patch.dict(os.environ, {"BDR_PORTABLE": "folder"}):
            self.assertNotIn(".bat", "\n".join(install.final_message(True)))

    def test_errors_are_reported_without_traceback(self):
        failures = [install.SetupError("x"), OSError(28, "No space left on device"), KeyError("work_dir"),
                    json.JSONDecodeError("Expecting value", "", 0), ValueError("x")]
        for failure in failures:
            stderr = io.StringIO()
            with mock.patch.object(install, "setup", side_effect=failure), contextlib.redirect_stderr(stderr):
                self.assertEqual(install.main([]), 1)
            self.assertIn("ERROR:", stderr.getvalue())

    def test_build_tree_from_another_checkout_is_detected(self):
        build = self.root / "build"
        build.mkdir()
        self.assertIsNone(install.foreign_source(build))
        self.assertIsNone(project.foreign_build_tree(build))
        cache = build / "CMakeCache.txt"
        here = install.ROOT.as_posix()
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={here}\n", encoding="utf-8")
        self.assertIsNone(install.foreign_source(build))
        self.assertIsNone(project.foreign_build_tree(build))
        # A tree configured from an older clone (iCloud copy) must be reconfigured.
        other = (self.root / "old clone").as_posix()
        cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={other}\n", encoding="utf-8")
        self.assertEqual(install.foreign_source(build), Path(other))
        self.assertEqual(project.foreign_build_tree(build), Path(other))

    def test_build_tree_follows_a_visual_studio_update(self):
        build = self.root / "build"
        build.mkdir()
        instance = self.root / "VS" / "Community"
        vswhere = self.root / "x86" / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        vswhere.parent.mkdir(parents=True)
        vswhere.write_bytes(b"")
        cache = build / "CMakeCache.txt"
        old = ("CMAKE_HOME_DIRECTORY:INTERNAL=x\r\n"
               f"CMAKE_GENERATOR_INSTANCE:UNINITIALIZED={instance.as_posix()},version=18.10.12306.391\r\n"
               "CMAKE_X:BOOL=ON\r\n").encode()
        cache.write_bytes(old)
        out = io.StringIO()
        with mock.patch.dict(os.environ, {"ProgramFiles(x86)": str(self.root / "x86")}), \
                mock.patch.object(project.subprocess, "run", return_value=mock.Mock(stdout="18.11.12328.437\n")) as ask, \
                contextlib.redirect_stdout(out):
            project.follow_visual_studio_update(build)  # instance gone: left for CMake to report
            self.assertEqual(cache.read_bytes(), old)
            ask.assert_not_called()
            instance.mkdir(parents=True)
            project.follow_visual_studio_update(build)
            self.assertEqual(cache.read_bytes(), old.replace(b"18.10.12306.391", b"18.11.12328.437"))
            self.assertEqual(ask.call_args[0][0][1:3], ["-path", instance.as_posix()])
            project.follow_visual_studio_update(build)  # already current
        self.assertEqual(out.getvalue().count("Visual Studio was updated"), 1)

    def test_compiler_choice_between_visual_studio_and_the_downloaded_one(self):
        # (Visual Studio, downloaded compiler ready, build tree, tree made with the downloaded one) -> (portable, fresh)
        cases = {(True, False, False, False): (False, False),  # Visual Studio, as before
                 (True, True, True, False): (False, False),    # a Visual Studio tree keeps Visual Studio
                 (True, True, True, True): (True, False),      # a tree of the downloaded compiler keeps it
                 (True, False, True, True): (False, True),     # ...until its folder is deleted
                 (False, False, False, False): (True, False),  # no Visual Studio: download
                 (False, True, True, False): (True, True),     # Visual Studio uninstalled: start the tree over
                 (False, False, True, True): (True, True)}     # downloaded again, maybe a newer compiler
        for given, expected in cases.items():
            self.assertEqual(install.choose_compiler(*given), expected, given)

    @unittest.skipUnless(os.name == "nt", "the downloaded compiler is for Windows")
    def test_without_visual_studio_the_compiler_is_offered_then_used(self):
        build, folder = self.work / "build", self.work / "toolchain"
        with mock.patch.object(install, "find_visual_studio", return_value=None):
            with self.assertRaises(install.SetupError) as raised:
                install.check_prerequisites(self.work, build, 0)
            self.assertTrue(raised.exception.compiler)
            self.assertEqual(raised.exception.problems, [])
            self.assertIn("--download-compiler", str(raised.exception))
            chosen = install.check_prerequisites(self.work, build, 0, download_compiler=True)
            self.assertEqual((chosen["portable"], chosen["generator"]), (folder, ["--generator", "Ninja"]))
            # A tree Visual Studio configured cannot change generator: it starts over.
            build.mkdir()
            (build / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n"
                                                  "CMAKE_COMMAND:INTERNAL=C:/VS/cmake.exe\n", encoding="utf-8")
            (folder / install.portable_toolchain.RECORD).parent.mkdir()
            (folder / install.portable_toolchain.RECORD).write_text("{}", encoding="utf-8")
            chosen = install.check_prerequisites(self.work, build, 0)
            self.assertEqual((chosen["fresh"], chosen["cmake"], chosen["generator"]),
                             (True, str(folder / "cmake" / "bin" / "cmake.exe"), ["--generator", "Ninja"]))

    def test_a_terminal_shows_the_licence_before_the_download(self):
        out = io.StringIO()
        terminal = mock.Mock(isatty=mock.Mock(return_value=True))
        with mock.patch.object(install.portable_toolchain, "licence", return_value="https://example.invalid/terms"), \
                mock.patch.object(install.sys, "stdin", terminal), contextlib.redirect_stdout(out):
            with mock.patch("builtins.input", return_value="y") as asked:
                self.assertTrue(install.accept_licence(self.work / "toolchain"))
            self.assertIn("https://example.invalid/terms", out.getvalue())
            self.assertIn("[y/N]", asked.call_args[0][0])
            with mock.patch("builtins.input", return_value=""):
                self.assertFalse(install.accept_licence(self.work / "toolchain"))
            terminal.isatty.return_value = False  # the launcher: it asks in its own window
            self.assertFalse(install.accept_licence(self.work / "toolchain"))

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
