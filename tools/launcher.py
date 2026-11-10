#!/usr/bin/env python3
"""Launcher window: the ISO, save profiles, installation, the game with its console,
the settings and updates from GitHub.

Usage: Launcher.bat on Windows, or  python3 tools/launcher.py
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import re
import signal
import subprocess
import sys
import threading
import traceback

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, simpledialog, ttk
except ImportError:  # the helpers below work, and are tested, without Tk
    tk = None

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import install  # noqa: E402
from project import DEFAULT_PROFILE, PROFILE_NAME  # noqa: E402

SETTINGS = ROOT / "settings.ini"
CHANGELOG = ROOT / "CHANGELOG.md"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # children never open a console window
SETTING_LINE = re.compile(r"\s*([a-z0-9_]+)\s*=\s*(.*?)\s*$")
# Settings with a fixed list of values; refresh_rate also takes any number typed in.
CHOICES = {"aspect": ("4:3", "16:9", "stretch"), "super_sampling": ("1", "2", "4", "8", "16"),
           "refresh_rate": ("50", "60", "120", "144", "165", "240", "300", "360", "500")}
CONSOLE_LINES = 5000


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


def git(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, encoding="utf-8", errors="replace",
                          timeout=120, creationflags=NO_WINDOW)


def update_state() -> dict:
    """{"error": text}, or {"behind": commits the GitHub branch is ahead by, "new": their changelog entries}."""
    if not (ROOT / ".git").exists():
        return {"error": "Updates need the Git copy of the project (git clone, see the README)."}
    try:
        fetch = git("fetch", "--quiet")
        if fetch.returncode:
            return {"error": fetch.stderr.strip() or "git fetch failed"}
        behind = int(git("rev-list", "--count", "HEAD..@{u}").stdout.strip() or 0)
        remote = git("show", "@{u}:CHANGELOG.md").stdout if behind else ""
    except (OSError, subprocess.TimeoutExpired, ValueError) as error:
        return {"error": f"git: {error}"}
    local = CHANGELOG.read_text(encoding="utf-8") if CHANGELOG.is_file() else ""
    return {"behind": behind, "new": new_entries(local, remote)}


def console_python() -> str:
    """python.exe next to the pythonw.exe that runs the launcher: children need their output."""
    exe = Path(sys.executable)
    console = exe.with_name(exe.name.lower().replace("pythonw", "python"))
    return str(console if console.is_file() else exe)


class Launcher:
    def __init__(self, install_now: bool = False) -> None:
        self.lock = install.read_json(ROOT / "project.json")
        self.work = work_dir()
        self.local = self.work / "local"
        self.root = tk.Tk()
        self.root.title("Burnout Dominator Reversed")
        self.root.geometry("940x660")
        self.root.minsize(760, 520)
        self.process: subprocess.Popen | None = None
        self.running = ""  # what self.process is, for the messages
        self.events: queue.Queue = queue.Queue()
        self.setting_vars: dict[str, tk.Variable] = {}

        self.tabs = ttk.Notebook(self.root)
        self.tabs.pack(fill="both", expand=True, padx=8, pady=8)
        self.build_play()
        self.build_console()
        self.build_settings()
        self.build_updates()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        # pythonw has no stderr: an error in a handler goes to the console tab.
        self.root.report_callback_exception = lambda *error: self.write("".join(traceback.format_exception(*error)))
        self.refresh()
        self.root.after(100, self.drain)
        threading.Thread(target=self.check_updates, args=(True,), daemon=True).start()
        if install_now and install_status(self.work, self.lock)["ISO found"]:
            self.install()

    # --- Play ---------------------------------------------------------------

    def build_play(self) -> None:
        tab = ttk.Frame(self.tabs, padding=12)
        self.tabs.add(tab, text="Play")

        disc = ttk.LabelFrame(tab, text="Game disc", padding=10)
        disc.pack(fill="x")
        recorded = install.recorded_iso(self.local)
        self.iso = tk.StringVar(value=str(recorded) if recorded else "")
        ttk.Entry(disc, textvariable=self.iso).grid(row=0, column=0, sticky="ew")
        ttk.Button(disc, text="Browse...", command=self.browse_iso).grid(row=0, column=1, padx=(8, 0))
        ttk.Label(disc, foreground="#555", text="Your own Burnout Dominator ISO, European version (SLES-54627). "
                                               "Keep it where it is: the game reads it at every launch.").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(6, 0))
        disc.columnconfigure(0, weight=1)

        setup = ttk.LabelFrame(tab, text="Installation", padding=10)
        setup.pack(fill="x", pady=12)
        self.steps: dict[str, ttk.Label] = {}
        for row, name in enumerate(install_status(self.work, self.lock)):
            self.steps[name] = ttk.Label(setup)
            self.steps[name].grid(row=row, column=0, sticky="w")
        self.install_button = ttk.Button(setup, text="Generate the code and build the game", command=self.install)
        self.install_button.grid(row=0, column=1, rowspan=2, padx=(24, 0), sticky="e")
        ttk.Label(setup, foreground="#555", text="30 to 90 minutes the first time,\na few minutes after an update.").grid(
            row=2, column=1, rowspan=2, padx=(24, 0), sticky="e")
        ttk.Label(setup, foreground="#555", text=f"Work folder: {self.work}").grid(row=4, column=0, columnspan=2,
                                                                                 sticky="w", pady=(6, 0))
        setup.columnconfigure(0, weight=1)

        saves = ttk.LabelFrame(tab, text="Save profile", padding=10)
        saves.pack(fill="x")
        self.profile = tk.StringVar(value=self.read_state().get("profile", DEFAULT_PROFILE))
        self.profile_box = ttk.Combobox(saves, textvariable=self.profile, state="readonly", width=30)
        self.profile_box.grid(row=0, column=0, sticky="w")
        self.profile_box.bind("<<ComboboxSelected>>", lambda event: self.write_state())
        ttk.Button(saves, text="New...", command=self.new_profile).grid(row=0, column=1, padx=8)
        ttk.Button(saves, text="Open its folder", command=self.open_profile).grid(row=0, column=2)
        ttk.Label(saves, foreground="#555", text="Each profile has its own memory card and save states.").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(6, 0))

        actions = ttk.Frame(tab)
        actions.pack(fill="x", pady=16)
        self.play_button = ttk.Button(actions, text="Play", command=self.play, width=18)
        self.play_button.pack(side="left", ipady=8)
        self.stop_button = ttk.Button(actions, text="Stop", command=self.stop)
        self.stop_button.pack(side="left", padx=8, ipady=8)
        self.state = ttk.Label(actions, foreground="#555")
        self.state.pack(side="left", padx=8)

    def browse_iso(self) -> None:
        path = filedialog.askopenfilename(title="Burnout Dominator ISO", filetypes=[("ISO image", "*.iso"), ("All files", "*")])
        if path:
            self.iso.set(str(Path(path)))

    def read_state(self) -> dict:
        try:
            return json.loads((self.local / "launcher.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}  # no choice remembered yet, or an unreadable file: the default profile

    def write_state(self) -> None:
        if self.local.is_dir():
            (self.local / "launcher.json").write_text(json.dumps({"profile": self.profile.get()}, indent=2) + "\n", encoding="utf-8")

    def profile_folder(self) -> Path:
        name = self.profile.get()
        return self.local / "saves" if name == DEFAULT_PROFILE else self.local / "profiles" / name

    def new_profile(self) -> None:
        name = simpledialog.askstring("New save profile", "Name of the profile:", parent=self.root)
        if name is None:
            return
        name = name.strip()
        if not PROFILE_NAME.fullmatch(name) or name in profiles(self.local):
            messagebox.showerror("New save profile", "Use a new name made of letters, digits, spaces, - and _ (40 at most).")
            return
        (self.local / "profiles" / name / "saves").mkdir(parents=True)
        self.profile.set(name)
        self.write_state()
        self.refresh()

    def open_profile(self) -> None:
        folder = self.profile_folder()
        folder.mkdir(parents=True, exist_ok=True)
        if os.name == "nt":
            os.startfile(folder)
        else:
            subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", str(folder)])

    def install(self) -> None:
        command = [console_python(), "-u", str(ROOT / "tools" / "install.py"), "--iso", self.iso.get().strip()]
        self.start("Installation", command)

    def play(self) -> None:
        self.write_state()
        command = [console_python(), "-u", str(ROOT / "tools" / "project.py"), "run", "--echo", "--status-ms", "5000",
                   "--profile", self.profile.get()]
        self.start("Game", command, lambda code: self.load_settings())  # F1 in the game rewrites settings.ini

    # --- Console ------------------------------------------------------------

    def build_console(self) -> None:
        tab = ttk.Frame(self.tabs, padding=8)
        self.tabs.add(tab, text="Console")
        self.console_tab = tab
        bar = ttk.Frame(tab)
        bar.pack(fill="x", pady=(0, 6))
        ttk.Button(bar, text="Stop", command=self.stop).pack(side="left")
        ttk.Button(bar, text="Copy all", command=self.copy_console).pack(side="left", padx=8)
        ttk.Button(bar, text="Clear", command=lambda: self.set_text(self.console, "")).pack(side="left")
        self.console = self.text_area(tab, font=("Consolas" if os.name == "nt" else "monospace", 9), wrap="none")

    def text_area(self, parent, **options) -> tk.Text:
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)
        text = tk.Text(frame, state="disabled", **options)
        scroll = ttk.Scrollbar(frame, command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        text.pack(side="left", fill="both", expand=True)
        return text

    def set_text(self, widget: tk.Text, content: str) -> None:
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("end", content)
        widget.configure(state="disabled")

    def write(self, line: str) -> None:
        self.console.configure(state="normal")
        self.console.insert("end", line)
        if int(self.console.index("end-1c").split(".")[0]) > CONSOLE_LINES:
            self.console.delete("1.0", f"{CONSOLE_LINES // 5}.0")
        self.console.configure(state="disabled")
        self.console.see("end")

    def copy_console(self) -> None:
        self.root.clipboard_clear()
        self.root.clipboard_append(self.console.get("1.0", "end"))

    def start(self, title: str, command: list[str], done=None) -> None:
        """Run one command at a time, its output in the console tab; done(exit code) follows."""
        if self.process:
            return
        self.write(f"\n=== {title} ===\n")
        self.tabs.select(self.console_tab)
        env = dict(os.environ, PYTHONUTF8="1")
        try:
            self.process = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, encoding="utf-8", errors="replace",
                                            creationflags=NO_WINDOW, start_new_session=os.name != "nt")
        except OSError as error:
            self.write(f"{command[0]}: {error}\n")
            return
        self.running = title
        threading.Thread(target=self.pump, args=(self.process, done), daemon=True).start()
        self.refresh()

    def pump(self, process: subprocess.Popen, done) -> None:
        for line in process.stdout:
            self.events.put(line)
        self.events.put(f"=== finished, exit code {process.wait()} ===\n")
        self.events.put(("done", done, process.returncode))

    def stop(self) -> None:
        """Stop the running command and everything it started (the game is a grandchild)."""
        if not self.process or self.process.poll() is not None:
            return
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(self.process.pid), "/T", "/F"], capture_output=True, creationflags=NO_WINDOW)
        else:
            os.killpg(self.process.pid, signal.SIGTERM)

    def drain(self) -> None:
        """Events from the worker threads, handled on the Tk thread."""
        try:
            while True:
                event = self.events.get_nowait()
                if isinstance(event, str):
                    self.write(event)
                elif event[0] == "done":
                    self.process = None
                    self.refresh()
                    if event[1]:
                        event[1](event[2])
                else:
                    self.show_updates(event[1], event[2])
        except queue.Empty:
            pass
        finally:
            self.root.after(100, self.drain)  # also after an error in a handler

    # --- Settings -----------------------------------------------------------

    def build_settings(self) -> None:
        tab = ttk.Frame(self.tabs, padding=8)
        self.tabs.add(tab, text="Settings")
        bar = ttk.Frame(tab)
        bar.pack(fill="x", pady=(0, 6))
        ttk.Button(bar, text="Save", command=self.save_settings).pack(side="left")
        ttk.Button(bar, text="Reload", command=self.load_settings).pack(side="left", padx=8)
        self.settings_note = ttk.Label(bar, foreground="#555")
        self.settings_note.pack(side="left", padx=8)
        canvas = tk.Canvas(tab, highlightthickness=0)
        scroll = ttk.Scrollbar(tab, command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        self.settings_frame = ttk.Frame(canvas)
        window = canvas.create_window((0, 0), window=self.settings_frame, anchor="nw")
        self.settings_frame.bind("<Configure>", lambda event: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda event: canvas.itemconfigure(window, width=event.width))
        canvas.bind("<Enter>", lambda event: canvas.bind_all("<MouseWheel>", lambda wheel: canvas.yview_scroll(-wheel.delta // 120, "units")))
        canvas.bind("<Leave>", lambda event: canvas.unbind_all("<MouseWheel>"))
        self.load_settings()

    def load_settings(self) -> None:
        for child in self.settings_frame.winfo_children():
            child.destroy()
        self.setting_vars.clear()
        if not SETTINGS.is_file():
            self.settings_note.configure(text="The game writes settings.ini at its first launch; they appear here afterwards.")
            return
        self.settings_note.configure(text="F1 in the game changes the same settings. Changes apply at the next launch.")
        for item in parse_settings(SETTINGS.read_text(encoding="utf-8")):
            if "comment" in item:
                ttk.Label(self.settings_frame, text=item["comment"], foreground="#555", wraplength=820, justify="left").pack(
                    fill="x", anchor="w", pady=(12, 2))
                continue
            key, value = item["key"], item["value"]
            row = ttk.Frame(self.settings_frame)
            row.pack(fill="x", anchor="w", pady=1)
            ttk.Label(row, text=key, width=22).pack(side="left")
            if value in ("true", "false"):
                var: tk.Variable = tk.BooleanVar(value=value == "true")
                ttk.Checkbutton(row, variable=var).pack(side="left")
            else:
                var = tk.StringVar(value=value)
                if key in CHOICES:
                    ttk.Combobox(row, textvariable=var, values=CHOICES[key], width=12,
                                 state="normal" if key == "refresh_rate" else "readonly").pack(side="left")
                else:
                    ttk.Entry(row, textvariable=var, width=44).pack(side="left")
            self.setting_vars[key] = var

    def save_settings(self) -> None:
        if not self.setting_vars:
            return
        values = {key: ("true" if var.get() else "false") if isinstance(var, tk.BooleanVar) else str(var.get()).strip()
                  for key, var in self.setting_vars.items()}
        SETTINGS.write_text(apply_settings(SETTINGS.read_text(encoding="utf-8"), values), encoding="utf-8")
        self.settings_note.configure(text="Saved. Changes apply at the next launch of the game.")

    # --- Updates ------------------------------------------------------------

    def build_updates(self) -> None:
        tab = ttk.Frame(self.tabs, padding=8)
        self.tabs.add(tab, text="Updates")
        self.updates_tab = tab
        bar = ttk.Frame(tab)
        bar.pack(fill="x", pady=(0, 6))
        ttk.Button(bar, text="Check for updates", command=lambda: threading.Thread(target=self.check_updates, daemon=True).start()).pack(side="left")
        self.update_button = ttk.Button(bar, text="Update now", command=self.update, state="disabled")
        self.update_button.pack(side="left", padx=8)
        self.update_note = ttk.Label(bar, foreground="#555")
        self.update_note.pack(side="left", padx=8)
        family = "Segoe UI" if os.name == "nt" else "sans-serif"
        self.changelog = self.text_area(tab, wrap="word", font=(family, 10), padx=10, pady=8, spacing1=3)
        self.changelog.tag_configure("title", font=(family, 13, "bold"))
        self.changelog.tag_configure("heading", font=(family, 10, "bold"))
        self.show_changelog("Installed version", changelog_entries(CHANGELOG.read_text(encoding="utf-8")) if CHANGELOG.is_file() else [])

    def show_changelog(self, title: str, entries: list[tuple[str, str]]) -> None:
        text = self.changelog
        text.configure(state="normal")
        text.delete("1.0", "end")
        text.insert("end", title + "\n", "title")
        for heading, body in entries:
            text.insert("end", f"\n{heading}\n", "heading")
            text.insert("end", body.replace("**", "").replace("`", "") + "\n")  # Markdown marks are not rendered
        text.configure(state="disabled")

    def check_updates(self, quiet: bool = False) -> None:
        self.events.put(("updates", update_state(), quiet))

    def show_updates(self, state: dict, quiet: bool) -> None:
        if "error" in state:
            if not quiet:
                self.update_note.configure(text=state["error"])
            return
        behind = state["behind"]
        self.update_button.configure(state="normal" if behind and not self.process else "disabled")
        self.tabs.tab(self.updates_tab, text="Updates (new)" if behind else "Updates")
        if not behind:
            self.update_note.configure(text="Up to date.")
            return
        self.update_note.configure(text=f"An update is available ({behind} change{'s' if behind > 1 else ''} on GitHub).")
        self.show_changelog("What the update brings", state["new"] or [("Technical changes", "Nothing a player would notice.")])

    def update(self) -> None:
        self.start("Update", ["git", "pull", "--ff-only"], self.updated)

    def updated(self, code: int) -> None:
        if code:
            messagebox.showerror("Update", "The update did not go through; the console tab says why.")
            return
        # The launcher itself may have changed: start the new one, which rebuilds the game.
        subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--install"], cwd=ROOT, creationflags=NO_WINDOW)
        self.root.destroy()

    # --- State --------------------------------------------------------------

    def refresh(self) -> None:
        status = install_status(self.work, self.lock)
        for name, done in status.items():
            self.steps[name].configure(text=("✔  " if done else "✖  ") + name, foreground="#1a7f37" if done else "#b42318")
        names = profiles(self.local)
        self.profile_box.configure(values=names)
        if self.profile.get() not in names:
            self.profile.set(DEFAULT_PROFILE)
        busy = self.process is not None
        self.install_button.configure(state="disabled" if busy else "normal")
        self.play_button.configure(state="normal" if all(status.values()) and not busy else "disabled")
        self.stop_button.configure(state="normal" if busy else "disabled")
        self.state.configure(text=f"{self.running} running: see the Console tab." if busy
                             else "Ready." if all(status.values()) else "Give the ISO, then generate the code and build the game.")

    def close(self) -> None:
        if self.process and self.process.poll() is None:
            if not messagebox.askyesno("Quit", f"{self.running} is still running. Stop it and quit?"):
                return
            self.stop()
        self.root.destroy()


def main() -> int:
    if tk is None:
        print("Tk is missing: install it (Linux: sudo apt install python3-tk) or use tools/install.py and tools/project.py run.",
              file=sys.stderr)
        return 1
    try:
        app = Launcher(install_now="--install" in sys.argv[1:])
    except install.SetupError as error:  # a work directory the installer refuses, an unreadable project file
        tk.Tk().withdraw()
        messagebox.showerror("Burnout Dominator Reversed", str(error))
        return 1
    app.root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
