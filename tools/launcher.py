#!/usr/bin/env python3
"""The launcher: the ISO, the installation, save profiles, the game with its console,
the settings and updates, on a local page shown in an app window.

Usage: BurnoutDominator.exe (portable installation), Launcher.bat, or  python3 tools/launcher.py
Python's standard library only: a portable installation runs it with the embeddable Python.
"""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import base64
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import webbrowser
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# A portable installation: one folder holds the launcher's file, this project (source/), the work
# directory and the settings, so that the folder can be moved. BurnoutDominator.exe/.py sets it.
PORTABLE = Path(os.environ["BDR_PORTABLE"]).resolve() if os.environ.get("BDR_PORTABLE") else None
if PORTABLE:
    os.environ["BDR_WORK_DIR"] = str(PORTABLE / "work")
    os.environ["BDR_SETTINGS"] = str(PORTABLE / "settings.ini")
    if (PORTABLE / "git" / "cmd").is_dir():  # the Git the Windows file brought, for the build's dependencies
        os.environ["PATH"] = str(PORTABLE / "git" / "cmd") + os.pathsep + os.environ.get("PATH", "")
sys.path.insert(0, str(ROOT / "tools"))
import install  # noqa: E402
from project import DEFAULT_PROFILE, PROFILE_NAME, settings_file  # noqa: E402

SETTINGS = settings_file()
CHANGELOG = ROOT / "CHANGELOG.md"
REPOSITORY = "literalynn/BurnoutDominatorReversed"
LATEST_RELEASE = f"https://api.github.com/repos/{REPOSITORY}/releases/latest"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # children never open a console window
SETTING_LINE = re.compile(r"\s*([a-z0-9_]+)\s*=\s*(.*?)\s*$")
# Settings with a fixed list of values; refresh_rate also takes any number typed in.
CONSOLE_LINES = 5000
PORTS = range(47800, 47810)  # a fixed first choice: the app window remembers its size per address


def work_dir() -> Path:
    """Where the game's files live: the installer's choice, known before the first installation."""
    return install.choose_work_dir(None, dict(os.environ), ROOT, Path.home())[0]


def install_status(work: Path, lock: dict) -> dict[str, bool]:
    """The steps of the installation, in order, and whether each is done."""
    local = work / "local"
    iso = install.recorded_iso(local)
    return {"ISO found": bool(iso and iso.is_file()),
            "Disc files extracted": (local / "disc" / lock["boot_path"]).is_file(),
            "Game code generated": (work / "generated" / "generation.json").is_file(),
            "Game built": install.find_game(work / "build") is not None}


def profiles(local: Path) -> list[str]:
    """Save profiles: the default one, then the folders of <work>/local/profiles."""
    folder = local / "profiles"
    names = sorted(p.name for p in folder.iterdir() if p.is_dir() and PROFILE_NAME.fullmatch(p.name)) if folder.is_dir() else []
    return [DEFAULT_PROFILE] + [name for name in names if name != DEFAULT_PROFILE]


def copy_saves(source: Path, profile: Path) -> None:
    """Copy the saves a player points at into a new profile's folder. `source` is a profile or work folder (it has
    "saves"), a memory card folder (it holds save folders) or one save folder (it holds icon.sys)."""
    if not source.is_dir():
        raise ValueError("This folder does not exist.")
    if (source / "saves").is_dir():
        for part in ("saves", "saves_slot1", "states"):
            if (source / part).is_dir():
                shutil.copytree(source / part, profile / part)
    elif (source / "icon.sys").is_file():
        shutil.copytree(source, profile / "saves" / source.name)
    elif any((entry / "icon.sys").is_file() for entry in source.iterdir() if entry.is_dir()):
        shutil.copytree(source, profile / "saves")
    else:
        raise ValueError("No PS2 save in this folder. Choose a save folder (it holds icon.sys), the folder above it, "
                         "or a profile's folder.")


def parse_settings(text: str) -> list[dict]:
    """settings.ini in file order: {"comment": text} for a block of comment lines,
    {"key": k, "value": v} for a setting."""
    items: list[dict] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            comment = stripped.lstrip("#").strip()
            if items and "comment" in items[-1] and not items[-1].get("closed"):
                items[-1]["comment"] += " " + comment
            else:
                items.append({"comment": comment})
        elif match := SETTING_LINE.match(line):
            items.append({"key": match.group(1), "value": match.group(2)})
        elif items and "comment" in items[-1]:
            items[-1]["closed"] = True  # a blank line ends the comment block
    return items


def apply_settings(text: str, values: dict[str, str]) -> str:
    """The same file with new values; comments, order and unknown lines are kept."""
    lines = []
    for line in text.splitlines():
        match = SETTING_LINE.match(line)
        if match and not line.lstrip().startswith("#") and match.group(1) in values:
            line = f"{match.group(1)} = {values[match.group(1)]}"
        lines.append(line)
    return "\n".join(lines) + "\n"


def changelog_entries(text: str) -> list[tuple[str, str]]:
    """(heading, body) of each "## " section of CHANGELOG.md, newest first as in the file."""
    entries: list[tuple[str, str]] = []
    for line in text.splitlines():
        if line.startswith("## "):
            entries.append((line[3:].strip(), ""))
        elif entries:
            entries[-1] = (entries[-1][0], entries[-1][1] + line + "\n")
    return [(heading, body.strip()) for heading, body in entries]


def new_entries(local: str, remote: str) -> list[tuple[str, str]]:
    """The entries of the remote changelog that the installed one does not have."""
    known = {heading for heading, _ in changelog_entries(local)}
    return [entry for entry in changelog_entries(remote) if entry[0] not in known]


def installed_version() -> str:
    """The version at the top of the installed CHANGELOG.md, "" without one."""
    entries = changelog_entries(CHANGELOG.read_text(encoding="utf-8")) if CHANGELOG.is_file() else []
    match = re.match(r"\d+(\.\d+)*", entries[0][0]) if entries else None
    return match.group(0) if match else ""


def version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r"\d+", version))


def git(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, encoding="utf-8", errors="replace",
                          timeout=120, creationflags=NO_WINDOW)


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "BurnoutDominatorReversed-launcher"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def update_state(download=fetch) -> dict:
    """{"error": text}, or {"behind": whether GitHub has something newer, "new": its changelog
    entries, "zip": the release's source archive when there is no Git copy to pull into}."""
    local = CHANGELOG.read_text(encoding="utf-8") if CHANGELOG.is_file() else ""
    if (ROOT / ".git").exists():
        try:
            fetched = git("fetch", "--quiet")
            if fetched.returncode:
                return {"error": fetched.stderr.strip() or "git fetch failed"}
            behind = int(git("rev-list", "--count", "HEAD..@{u}").stdout.strip() or 0)
            remote = git("show", "@{u}:CHANGELOG.md").stdout if behind else ""
        except (OSError, subprocess.TimeoutExpired, ValueError) as error:
            return {"error": f"git: {error}"}
        return {"behind": behind > 0, "new": new_entries(local, remote)}
    if not PORTABLE or ROOT != PORTABLE / "source":
        return {"error": "Updates need the portable launcher (BurnoutDominator.exe) or a Git copy of the project."}
    try:
        release = json.loads(download(LATEST_RELEASE))
        tag = release["tag_name"]
        if version_key(tag) <= version_key(installed_version()):
            return {"behind": False, "new": []}
        archive = next(asset["browser_download_url"] for asset in release["assets"] if asset["name"] == "source.zip")
        remote = download(f"https://raw.githubusercontent.com/{REPOSITORY}/{tag}/CHANGELOG.md").decode("utf-8")
    except (OSError, ValueError, KeyError, StopIteration) as error:
        return {"error": f"GitHub could not be read: {error}"}
    return {"behind": True, "new": new_entries(local, remote), "zip": archive}


def sync_zip(data: bytes, target: Path) -> tuple[int, int]:
    """Make `target` hold exactly the archive's files; return (files written, files removed).
    A file whose bytes did not change is left alone, so that the build only compiles what changed.
    Everything else in `target` is deleted: only for a folder that holds nothing but the project."""
    target = target.resolve()
    written, wanted = 0, set()
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for entry in archive.infolist():
            if entry.is_dir():
                continue
            path = (target / entry.filename).resolve()
            if target not in path.parents:
                raise ValueError(f"unsafe path in the archive: {entry.filename}")
            wanted.add(path)
            content = archive.read(entry)
            if not path.is_file() or path.read_bytes() != content:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
                written += 1
    stale = [path for path in target.rglob("*") if path.is_file() and path not in wanted and "__pycache__" not in path.parts]
    for path in stale:
        path.unlink()
    return written, len(stale)


def console_python() -> str:
    """python.exe next to the pythonw.exe that runs the launcher: children need their output."""
    exe = Path(sys.executable)
    console = exe.with_name(exe.name.lower().replace("pythonw", "python"))
    return str(console if console.is_file() else exe)


def choose(folder: bool = False) -> str:
    """A native dialog for the ISO, or for a folder of saves; "" when cancelled or when the system has none we know."""
    if os.name == "nt":
        # The dialog belongs to the launcher's window, where the click just came from: without an owner it
        # opens behind that window. Another program's window in front is left alone.
        user32, title = ctypes.windll.user32, ctypes.create_unicode_buffer(64)
        owner = user32.GetForegroundWindow()
        user32.GetWindowTextW(owner, title, 64)
        owner = owner if title.value.startswith("Burnout Dominator Reversed") else 0
        kind, setup, picked = (("FolderBrowserDialog", "$dialog.Description = 'The folder that holds your saves'", "SelectedPath") if folder else
                               ("OpenFileDialog", "$dialog.Title = 'Burnout Dominator ISO'; $dialog.Filter = 'ISO image (*.iso)|*.iso|All files|*.*'", "FileName"))
        script = ("Add-Type -AssemblyName System.Windows.Forms; $owner = New-Object System.Windows.Forms.NativeWindow;"
                  f"$owner.AssignHandle([IntPtr]{owner});"
                  f"$dialog = New-Object System.Windows.Forms.{kind}; {setup};"
                  f"if ($dialog.ShowDialog($owner) -eq 'OK') {{ [Console]::OutputEncoding = [Text.Encoding]::UTF8; $dialog.{picked} }}")
        command = ["powershell", "-NoProfile", "-Command", script]
    elif shutil.which("zenity"):
        command = ["zenity", "--file-selection"] + (["--directory", "--title=The folder that holds your saves"] if folder else
                                                    ["--title=Burnout Dominator ISO", "--file-filter=*.iso"])
    else:
        return ""
    result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="replace", creationflags=NO_WINDOW)
    return result.stdout.strip()


def find_edge() -> str | None:
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), os.environ.get("LocalAppData")):
        candidate = Path(base) / "Microsoft" / "Edge" / "Application" / "msedge.exe" if base else None
        if candidate and candidate.is_file():
            return str(candidate)
    return None


def open_window(url: str) -> None:
    """An app window without browser controls where Edge offers one, else a browser tab."""
    edge = find_edge() if os.name == "nt" else None
    if edge:
        subprocess.Popen([edge, f"--app={url}", "--window-size=1280,720"])  # the page sets this size itself the first time
    else:
        webbrowser.open(url)


class Launcher:
    """The state behind the page. Worker threads change it under `changed` and notify the page's event streams."""

    def __init__(self, token: str | None = None) -> None:
        self.project = install.read_json(ROOT / "project.json")
        self.work = work_dir()
        self.local = self.work / "local"
        self.token = token or secrets.token_hex(16)
        self.changed = threading.Condition()
        self.lines: list[str] = []  # the console; line number = first + index
        self.first = 0
        self.revision = 0  # bumped whenever the page should read the state again
        self.process: subprocess.Popen | None = None
        self.running = ""  # what self.process is, for the page
        self.phase = ""  # the installer's last "==> ..." line: the step it is at
        self.updates: dict = {"note": "Not checked yet.", "behind": False, "new": []}
        self.clients = 0
        self.client_seen = time.monotonic()
        self.restart: list[str] | None = None  # set to the new launcher's arguments after an update
        try:
            install.check_prerequisites(self.work, self.work / "build", 0)
            self.missing = ""
        except install.SetupError as error:
            self.missing = "\n".join(getattr(error, "problems", [str(error)])) + "\nInstall what is missing, then start the launcher again."

    # --- State --------------------------------------------------------------

    def touch(self) -> None:
        with self.changed:
            self.revision += 1
            self.changed.notify_all()

    def write(self, text: str) -> None:
        with self.changed:
            self.lines.append(text)
            if text.startswith("==> "):
                self.phase = text[4:].strip()
                self.revision += 1
            if len(self.lines) > CONSOLE_LINES:
                drop = CONSOLE_LINES // 5
                del self.lines[:drop]
                self.first += drop
            self.changed.notify_all()

    def saved(self) -> dict:
        try:
            return json.loads((self.local / "launcher.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}  # no choice remembered yet, or an unreadable file: the default profile

    def profile(self) -> str:
        name = self.saved().get("profile", DEFAULT_PROFILE)
        return name if name in profiles(self.local) else DEFAULT_PROFILE

    def state(self) -> dict:
        status = install_status(self.work, self.project)
        recorded = install.recorded_iso(self.local)
        settings = parse_settings(SETTINGS.read_text(encoding="utf-8")) if SETTINGS.is_file() else []
        for item in settings:
            item.pop("closed", None)
        entries = changelog_entries(CHANGELOG.read_text(encoding="utf-8")) if CHANGELOG.is_file() else []
        return {"iso": str(recorded or ""), "steps": status, "ready": all(status.values()),
                "running": self.running if self.process else "", "phase": self.phase if self.process else "",
                "work": str(self.work),
                "profiles": profiles(self.local), "profile": self.profile(),
                "settings": settings, "settings_stamp": SETTINGS.stat().st_mtime_ns if SETTINGS.is_file() else 0,
                "version": installed_version(), "changelog": entries, "updates": self.updates,
                "missing": self.missing, "build": BUILD}

    # --- Commands -----------------------------------------------------------

    def start(self, title: str, command: list[str], done=None) -> None:
        """Run one command at a time, its output in the console; done(exit code) follows."""
        if self.process:
            raise ValueError(f"{self.running} is still running.")
        self.write(f"\n=== {title} ===\n")
        process = subprocess.Popen(command, cwd=ROOT, env=dict(os.environ, PYTHONUTF8="1"), stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                                   creationflags=NO_WINDOW, start_new_session=os.name != "nt")
        self.process, self.running, self.phase = process, title, ""

        def pump() -> None:
            for line in process.stdout:
                self.write(line)
            code = process.wait()
            self.write(f"=== finished, exit code {code} ===\n")
            self.process = None
            if done:
                done(code)
            self.touch()

        threading.Thread(target=pump, daemon=True).start()
        self.touch()

    def stop(self) -> None:
        """Stop the running command and everything it started (the game is a grandchild)."""
        process = self.process
        if not process or process.poll() is not None:
            return
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, creationflags=NO_WINDOW)
        else:
            os.killpg(process.pid, signal.SIGTERM)

    def install(self, iso: str = "") -> None:
        self.start("Installation", [console_python(), "-u", str(ROOT / "tools" / "install.py"), "--iso", iso.strip()])

    def play(self, profile: str) -> None:
        if profile not in profiles(self.local):
            raise ValueError("Unknown save profile.")
        self.select(profile)
        self.start("Game", [console_python(), "-u", str(ROOT / "tools" / "project.py"), "run", "--echo", "--status-ms", "5000",
                            "--profile", profile])

    def select(self, profile: str) -> None:
        if profile in profiles(self.local) and self.local.is_dir():
            (self.local / "launcher.json").write_text(json.dumps({"profile": profile}, indent=2) + "\n", encoding="utf-8")

    def new_profile(self, name: str, saves: str = "") -> None:
        """A new profile, empty or holding a copy of the saves in the folder `saves`."""
        name = name.strip()
        if not PROFILE_NAME.fullmatch(name) or name in profiles(self.local):
            raise ValueError("Use a new name made of letters, digits, spaces, - and _ (40 at most).")
        target = self.local / "profiles" / name
        if saves.strip():
            try:
                copy_saves(Path(saves.strip().strip('"')), target)
            except (ValueError, OSError):
                shutil.rmtree(target, ignore_errors=True)  # no half-copied profile
                raise
        (target / "saves").mkdir(parents=True, exist_ok=True)
        self.select(name)

    def open_profile(self, profile: str) -> None:
        if profile not in profiles(self.local):
            raise ValueError("Unknown save profile.")
        folder = self.local / "saves" if profile == DEFAULT_PROFILE else self.local / "profiles" / profile
        folder.mkdir(parents=True, exist_ok=True)
        if os.name == "nt":
            os.startfile(folder)
        else:
            subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", str(folder)])

    def save_settings(self, values: dict) -> None:
        if SETTINGS.is_file():
            clean = {str(key): str(value).replace("\n", " ").strip() for key, value in values.items()}
            with self.changed:  # one request at a time reads the file and writes it back
                SETTINGS.write_text(apply_settings(SETTINGS.read_text(encoding="utf-8"), clean), encoding="utf-8")

    # --- Updates ------------------------------------------------------------

    def check_updates(self) -> None:
        found = update_state()
        if "error" in found:
            self.updates = {"note": found["error"], "behind": False, "new": []}
        elif not found["behind"]:
            self.updates = {"note": "Up to date.", "behind": False, "new": []}
        else:
            entries = found["new"] or [("Technical changes", "Nothing a player would notice.")]
            self.updates = {"note": "An update is available.", "behind": True, "new": entries, "zip": found.get("zip", "")}
        self.touch()

    def update(self) -> None:
        """Bring the project up to date, then let a new launcher (this file may have changed) rebuild the game."""
        if self.process:
            raise ValueError(f"{self.running} is still running.")
        if (ROOT / ".git").exists():
            self.start("Update", ["git", "pull", "--ff-only"], lambda code: self.updated(code == 0))
            return
        archive = self.updates.get("zip")
        if not archive:
            raise ValueError("Check for updates first.")

        def download() -> None:
            self.write(f"\n=== Update ===\nDownloading {archive}\n")
            try:
                written, removed = sync_zip(fetch(archive), ROOT)
                self.write(f"{written} files changed, {removed} removed.\n")
                self.updated(True)
            except (OSError, ValueError, zipfile.BadZipFile) as error:
                self.write(f"The update did not go through: {error}\n")
            self.touch()

        threading.Thread(target=download, daemon=True).start()

    def updated(self, success: bool) -> None:
        if success:
            self.restart = ["--install"] if install_status(self.work, self.project)["ISO found"] else []
            self.touch()

    # --- Requests -----------------------------------------------------------

    def command(self, name: str, body: dict) -> dict:
        """One request of the page; the answer is the new state, or {"error": text}."""
        try:
            if name in ("browse", "browse-saves"):
                return {"path": choose(folder=name == "browse-saves")}
            if name == "install":
                self.install(str(body.get("iso", "")))
            elif name == "play":
                self.play(str(body.get("profile", "")))
            elif name == "stop":
                self.stop()
            elif name == "select":
                self.select(str(body.get("profile", "")))
            elif name == "new-profile":
                self.new_profile(str(body.get("name", "")), str(body.get("saves", "")))
            elif name == "open-profile":
                self.open_profile(str(body.get("profile", "")))
            elif name == "settings":
                self.save_settings(dict(body.get("values", {})))
            elif name == "check-updates":
                threading.Thread(target=self.check_updates, daemon=True).start()
            elif name == "update":
                self.update()
            elif name != "state":
                return {"error": f"Unknown command: {name}"}
        except (ValueError, OSError) as error:
            return {"error": str(error)}
        return self.state()


def handler(app: Launcher, port: int):
    hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *arguments) -> None:  # pythonw has no stderr
            pass

        def handle(self) -> None:
            try:
                super().handle()
            except ConnectionError:
                pass  # the page closed: nothing to report in the terminal

        def allowed(self, token: str) -> bool:
            # Only this machine's launcher page: the address must be ours (a web page elsewhere
            # cannot name it through another host), and the token is only in the page we served.
            return self.headers.get("Host") in hosts and secrets.compare_digest(token or "", app.token)

        def send(self, status: int, body: bytes, content: str) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            path, _, query = self.path.partition("?")
            token = dict(pair.partition("=")[::2] for pair in query.split("&")).get("t", "")
            if path == "/ping":
                self.send(200, json.dumps({"app": "bdr-launcher", "root": str(PORTABLE or ROOT)}).encode(), "application/json")
            elif not self.allowed(token):
                self.send(403, b"Open the launcher from its file.", "text/plain")
            elif path == "/":
                self.send(200, PAGE.replace("__TOKEN__", app.token).encode("utf-8"), "text/html; charset=utf-8")
            elif path == "/events":
                self.events()
            else:
                self.send(404, b"Not found.", "text/plain")

        def do_POST(self) -> None:
            if not self.allowed(self.headers.get("X-BDR-Token", "")):
                self.send(403, b"Forbidden.", "text/plain")
                return
            try:
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0") or 0)) or b"{}")
            except ValueError:
                body = {}
            answer = app.command(self.path.removeprefix("/api/"), body if isinstance(body, dict) else {})
            self.send(200, json.dumps(answer).encode("utf-8"), "application/json")

        def events(self) -> None:
            """Server-sent events: console lines as they come, and a word when the state changed."""
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            line, revision = app.first, -1
            with app.changed:
                app.clients += 1
            try:
                while True:
                    with app.changed:
                        if line >= app.first + len(app.lines) and revision == app.revision:
                            app.changed.wait(2)
                        line = max(line, app.first)
                        fresh = app.lines[line - app.first:]
                        line += len(fresh)
                        state, revision = revision != app.revision, app.revision
                    message = {"lines": fresh, "state": state} if fresh or state else None
                    # A comment line keeps the connection checked: writing to a closed page fails.
                    self.wfile.write((f"data: {json.dumps(message)}\n\n" if message else ": waiting\n\n").encode("utf-8"))
                    self.wfile.flush()
            except OSError:
                pass
            finally:
                with app.changed:
                    app.clients -= 1
                    app.client_seen = time.monotonic()

    return Handler


def session_file() -> Path:
    """Where a running launcher of this folder says how to reach it."""
    name = hashlib.sha256(str(PORTABLE or ROOT).encode("utf-8")).hexdigest()[:16]
    return Path(tempfile.gettempdir()) / f"bdr-launcher-{name}.json"


def running_instance() -> str | None:
    """The page address of a launcher already running for this folder."""
    try:
        session = json.loads(session_file().read_text(encoding="utf-8"))
        with urllib.request.urlopen(f"http://127.0.0.1:{session['port']}/ping", timeout=2) as response:
            if json.loads(response.read()).get("root") == str(PORTABLE or ROOT):
                return f"http://127.0.0.1:{session['port']}/?t={session['token']}"
    except (OSError, ValueError, KeyError):
        pass
    return None


def serve(arguments: list[str]) -> int:
    existing = running_instance()
    if existing and "--no-window" not in arguments and "--token" not in arguments:
        open_window(existing)
        return 0
    token = arguments[arguments.index("--token") + 1] if "--token" in arguments else None
    wanted = [int(arguments[arguments.index("--port") + 1])] if "--port" in arguments else list(PORTS) + [0]
    app = Launcher(token)
    server = None
    for port in wanted:
        for _ in range(20 if "--token" in arguments else 1):  # after an update the old launcher is still closing
            try:
                server = ThreadingHTTPServer(("127.0.0.1", port), BaseHTTPRequestHandler)
                break
            except OSError:
                time.sleep(0.25)
        if server:
            break
    if not server:
        return 1
    port = server.server_address[1]
    server.RequestHandlerClass = handler(app, port)
    server.daemon_threads = True
    session_file().write_text(json.dumps({"port": port, "token": app.token}), encoding="utf-8")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{port}/?t={app.token}"
    print(url, flush=True)
    if "--no-window" not in arguments and "--token" not in arguments:
        open_window(url)
    threading.Thread(target=app.check_updates, daemon=True).start()
    if "--install" in arguments and install_status(app.work, app.project)["ISO found"]:
        app.install()
    started = time.monotonic()
    while True:
        time.sleep(0.5)
        if app.restart is not None:
            # The project changed under us: a new launcher takes over the same address and token,
            # so the open page just carries on (it reloads itself when it sees the new build).
            server.shutdown()
            server.server_close()
            subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--port", str(port), "--token", app.token, *app.restart],
                             cwd=ROOT, creationflags=NO_WINDOW)
            return 0
        idle = time.monotonic() - max(app.client_seen, started)
        seen = app.client_seen > started or app.clients
        if "--no-window" not in arguments and not app.clients and not app.process and idle > (4 if seen else 90):
            session_file().unlink(missing_ok=True)
            return 0  # the window was closed


PAGE = Path(__file__).with_name("launcher.html").read_text(encoding="utf-8").replace(
    "__ICON__", base64.b64encode(Path(__file__).with_name("launcher.png").read_bytes()).decode("ascii"))
BUILD = hashlib.sha256((PAGE + Path(__file__).read_text(encoding="utf-8")).encode("utf-8")).hexdigest()[:12]


def main() -> int:
    try:
        return serve(sys.argv[1:])
    except install.SetupError as error:  # a work directory the installer refuses, an unreadable project file
        if os.name == "nt":
            import ctypes
            ctypes.windll.user32.MessageBoxW(None, str(error), "Burnout Dominator Reversed", 0x10)
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
