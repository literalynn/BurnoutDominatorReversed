import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import launcher  # noqa: E402
import project  # noqa: E402

SETTINGS = """# Burnout Dominator Reversed settings.

# Picture shape: 4:3, 16:9 or stretch.
# Second line of the same comment.
aspect = 4:3
smooth_scaling = true

# Sound volume in percent.
volume = 20
gamepad_mappings =
"""


class LauncherTests(unittest.TestCase):
    def test_settings_are_read_in_file_order_with_their_comments(self):
        items = launcher.parse_settings(SETTINGS)
        self.assertEqual(items[0]["comment"], "Burnout Dominator Reversed settings.")
        self.assertEqual(items[1]["comment"], "Picture shape: 4:3, 16:9 or stretch. Second line of the same comment.")
        self.assertEqual([(i["key"], i["value"]) for i in items if "key" in i],
                         [("aspect", "4:3"), ("smooth_scaling", "true"), ("volume", "20"), ("gamepad_mappings", "")])

    def test_saving_changes_values_and_keeps_everything_else(self):
        text = launcher.apply_settings(SETTINGS, {"aspect": "16:9", "volume": "55"})
        self.assertEqual(text, SETTINGS.replace("aspect = 4:3", "aspect = 16:9").replace("volume = 20", "volume = 55"))
        self.assertEqual(launcher.apply_settings(SETTINGS, {}), SETTINGS)

    def test_update_shows_the_changelog_entries_the_installed_version_lacks(self):
        local = "# Changelog\n\n## 1.0.0 (November 10, 2026)\n\n- The launcher.\n"
        remote = "# Changelog\n\n## 1.1.0 (December 1, 2026)\n\n- Faster.\n- Nicer.\n\n## 1.0.0 (November 10, 2026)\n\n- The launcher.\n"
        self.assertEqual(launcher.new_entries(local, remote), [("1.1.0 (December 1, 2026)", "- Faster.\n- Nicer.")])
        self.assertEqual(launcher.new_entries(remote, remote), [])

    def test_profiles_have_their_own_save_folders(self):
        with tempfile.TemporaryDirectory() as temp:
            local = Path(temp)
            self.assertEqual(launcher.profiles(local), ["Default"])
            (local / "profiles" / "Second run" / "saves").mkdir(parents=True)
            (local / "profiles" / "bad;name").mkdir()
            self.assertEqual(launcher.profiles(local), ["Default", "Second run"])
        self.assertEqual(project.save_dir(None), project.LOCAL / "saves")
        self.assertEqual(project.save_dir("Default"), project.LOCAL / "saves")
        self.assertEqual(project.save_dir("Second run"), project.LOCAL / "profiles" / "Second run" / "saves")
        with self.assertRaises(RuntimeError):
            project.save_dir("../elsewhere")

    @unittest.skipUnless(shutil.which("git"), "needs git")
    def test_update_check_reads_a_real_remote(self):
        def git(folder, *arguments):
            subprocess.run(["git", "-c", "user.name=test", "-c", "user.email=test@example.invalid", *arguments],
                           cwd=folder, check=True, capture_output=True)

        with tempfile.TemporaryDirectory() as temp:
            origin, local = Path(temp) / "origin", Path(temp) / "local"
            origin.mkdir()
            git(origin, "init", "--quiet", "--initial-branch=main")
            (origin / "CHANGELOG.md").write_text("# Changelog\n\n## 1.0.0\n\n- First.\n", encoding="utf-8")
            git(origin, "add", ".")
            git(origin, "commit", "--quiet", "-m", "one")
            git(temp, "clone", "--quiet", str(origin), str(local))
            with mock.patch.object(launcher, "ROOT", local), mock.patch.object(launcher, "CHANGELOG", local / "CHANGELOG.md"):
                self.assertEqual(launcher.update_state(), {"behind": 0, "new": []})
                (origin / "CHANGELOG.md").write_text("# Changelog\n\n## 1.1.0\n\n- Second.\n\n## 1.0.0\n\n- First.\n", encoding="utf-8")
                git(origin, "commit", "--quiet", "-am", "two")
                self.assertEqual(launcher.update_state(), {"behind": 1, "new": [("1.1.0", "- Second.")]})
                git(local, "pull", "--quiet", "--ff-only")  # what the Update button runs
                self.assertEqual(launcher.update_state(), {"behind": 0, "new": []})
            with mock.patch.object(launcher, "ROOT", Path(temp)):
                self.assertIn("error", launcher.update_state())

    def test_installation_steps_follow_the_files(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            lock = {"boot_path": "SLES_546.27"}
            self.assertEqual(list(launcher.install_status(work, lock).values()), [False, False, False, False])
            (work / "generated").mkdir()
            (work / "generated" / "generation.json").write_text("{}", encoding="utf-8")
            self.assertEqual(launcher.install_status(work, lock)["Game code generated"], True)


if __name__ == "__main__":
    unittest.main()
