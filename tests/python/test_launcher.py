import io
import json
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import zipfile
from http.server import ThreadingHTTPServer
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


def archive(files: dict[str, str]) -> bytes:
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as output:
        for name, text in files.items():
            output.writestr(name, text)
    return data.getvalue()


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
        self.assertLess(launcher.version_key("1.9.0"), launcher.version_key("v1.10.0"))

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
    def test_update_check_reads_a_real_git_remote(self):
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
                self.assertEqual(launcher.update_state(), {"behind": False, "new": []})
                (origin / "CHANGELOG.md").write_text("# Changelog\n\n## 1.1.0\n\n- Second.\n\n## 1.0.0\n\n- First.\n", encoding="utf-8")
                git(origin, "commit", "--quiet", "-am", "two")
                self.assertEqual(launcher.update_state(), {"behind": True, "new": [("1.1.0", "- Second.")]})
                git(local, "pull", "--quiet", "--ff-only")  # what the Update button runs
                self.assertEqual(launcher.update_state(), {"behind": False, "new": []})

    def test_portable_update_check_reads_the_latest_release(self):
        with tempfile.TemporaryDirectory() as temp:
            portable = Path(temp).resolve()
            source = portable / "source"
            source.mkdir()
            (source / "CHANGELOG.md").write_text("# Changelog\n\n## 1.0.0\n\n- First.\n", encoding="utf-8")
            pages = {launcher.LATEST_RELEASE: json.dumps({"tag_name": "v1.1.0", "assets": [
                         {"name": "BurnoutDominator.exe", "browser_download_url": "https://example.invalid/exe"},
                         {"name": "source.zip", "browser_download_url": "https://example.invalid/source.zip"}]}).encode(),
                     f"https://raw.githubusercontent.com/{launcher.REPOSITORY}/v1.1.0/CHANGELOG.md":
                         b"# Changelog\n\n## 1.1.0\n\n- Second.\n\n## 1.0.0\n\n- First.\n"}
            with mock.patch.object(launcher, "ROOT", source), mock.patch.object(launcher, "CHANGELOG", source / "CHANGELOG.md"), \
                    mock.patch.object(launcher, "PORTABLE", portable):
                self.assertEqual(launcher.update_state(pages.__getitem__),
                                 {"behind": True, "new": [("1.1.0", "- Second.")], "zip": "https://example.invalid/source.zip"})
                pages[launcher.LATEST_RELEASE] = json.dumps({"tag_name": "v1.0.0", "assets": []}).encode()
                self.assertEqual(launcher.update_state(pages.__getitem__), {"behind": False, "new": []})
                self.assertIn("error", launcher.update_state(lambda url: (_ for _ in ()).throw(OSError("offline"))))
            # A copy that is neither a Git checkout nor a portable installation is not replaced file by file.
            with mock.patch.object(launcher, "ROOT", source), mock.patch.object(launcher, "PORTABLE", None):
                self.assertIn("error", launcher.update_state(pages.__getitem__))

    def test_an_update_rewrites_only_the_files_that_changed(self):
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "source"
            self.assertEqual(launcher.sync_zip(archive({"a.txt": "one", "tools/b.py": "two", "old.txt": "x"}), target), (3, 0))
            kept = (target / "a.txt").stat().st_mtime_ns
            (target / "tools" / "__pycache__").mkdir()
            (target / "tools" / "__pycache__" / "b.pyc").write_bytes(b"cache")
            self.assertEqual(launcher.sync_zip(archive({"a.txt": "one", "tools/b.py": "three", "new.txt": "n"}), target), (2, 1))
            self.assertEqual((target / "a.txt").stat().st_mtime_ns, kept)
            self.assertEqual((target / "tools" / "b.py").read_text(), "three")
            self.assertFalse((target / "old.txt").exists())
            self.assertTrue((target / "tools" / "__pycache__" / "b.pyc").exists())
            with self.assertRaises(ValueError):
                launcher.sync_zip(archive({"../outside.txt": "no"}), target)
            self.assertFalse((Path(temp) / "outside.txt").exists())

    def test_the_one_file_downloads_the_project_next_to_itself_and_starts_its_launcher(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp).resolve() / "game"
            folder.mkdir()
            shutil.copy(ROOT / "tools" / "portable" / "BurnoutDominator.py", folder)
            # A stand-in for the project: its launcher only notes where it was told to keep everything.
            release = Path(temp) / "source.zip"
            release.write_bytes(archive({"tools/launcher.py": "import os, pathlib\n"
                                         "pathlib.Path(os.environ['BDR_PORTABLE'], 'started.txt').write_text(os.environ['BDR_PORTABLE'])\n"}))
            run = subprocess.run([sys.executable, str(folder / "BurnoutDominator.py")], capture_output=True, text=True,
                                 env={**__import__("os").environ, "BDR_SOURCE_URL": release.as_uri()})
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual((folder / "started.txt").read_text(), str(folder))
            self.assertTrue((folder / "source" / "tools" / "launcher.py").is_file())

    def test_installation_steps_follow_the_files(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            lock = {"boot_path": "SLES_546.27"}
            self.assertEqual(list(launcher.install_status(work, lock).values()), [False, False, False, False])
            (work / "generated").mkdir()
            (work / "generated" / "generation.json").write_text("{}", encoding="utf-8")
            self.assertEqual(launcher.install_status(work, lock)["Game code generated"], True)

    def test_the_page_and_its_commands_need_the_token_and_our_address(self):
        app = launcher.Launcher("secret")
        server = ThreadingHTTPServer(("127.0.0.1", 0), launcher.BaseHTTPRequestHandler)
        port = server.server_address[1]
        server.RequestHandlerClass = launcher.handler(app, port)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)

        def get(path, **headers):
            try:
                with urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=headers), timeout=10) as response:
                    return response.status, response.read()
            except urllib.error.HTTPError as error:
                return error.code, b""

        def post(command, token, body):
            request = urllib.request.Request(f"http://127.0.0.1:{port}/api/{command}", data=json.dumps(body).encode(),
                                             headers={"X-BDR-Token": token}, method="POST")
            try:
                with urllib.request.urlopen(request, timeout=10) as response:
                    return response.status, json.loads(response.read())
            except urllib.error.HTTPError as error:
                return error.code, {}

        self.assertEqual(get("/")[0], 403)
        self.assertEqual(get("/?t=wrong")[0], 403)
        self.assertEqual(get("/?t=secret", Host="evil.example")[0], 403)
        status, page = get("/?t=secret")
        self.assertEqual(status, 200)
        self.assertIn(b'const TOKEN = "secret"', page)
        self.assertEqual(post("state", "wrong", {})[0], 403)
        status, state = post("state", "secret", {})
        self.assertEqual(status, 200)
        self.assertEqual(list(state["steps"]), ["ISO found", "Disc files extracted", "Game code generated", "Game built"])
        self.assertIn("error", post("play", "secret", {"profile": "no such profile"})[1])
        self.assertIn("error", post("new-profile", "secret", {"name": "../x"})[1])
        self.assertIn("error", post("no-such-command", "secret", {})[1])


if __name__ == "__main__":
    unittest.main()
