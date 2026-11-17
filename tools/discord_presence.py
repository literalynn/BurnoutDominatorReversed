"""Discord's "Playing Burnout Dominator" while the game runs from the launcher, with the time since it started.

It talks to the Discord app on this computer over its local IPC: a named pipe on Windows, a Unix socket elsewhere,
carrying frames of a little-endian opcode and length, then JSON. It sends the activity and nothing else (no profile
name, no path), from a thread of its own: when Discord is not running or quits, the launcher and the game carry on,
and it tries again RETRY seconds later. Python's standard library only.
"""
from __future__ import annotations

import json
import os
import socket
import struct
import threading
import uuid

HANDSHAKE, FRAME, CLOSE, PING, PONG = range(5)
RETRY = 30  # seconds; Discord takes at most five activity updates in 20 seconds


def frame(opcode: int, message: dict) -> bytes:
    data = json.dumps(message).encode("utf-8")
    return struct.pack("<II", opcode, len(data)) + data


def read_exactly(stream, size: int) -> bytes:
    data = b""
    while len(data) < size:
        part = stream.read(size - len(data))
        if not part:
            raise OSError("Discord closed the connection")
        data += part
    return data


def read_frame(stream) -> tuple[int, dict]:
    opcode, length = struct.unpack("<II", read_exactly(stream, 8))
    return opcode, json.loads(read_exactly(stream, length))


def addresses() -> list[str]:
    """Where a Discord app listens: ten numbered pipes or sockets, the first free one taken. On Linux the Snap and
    Flatpak packages put theirs in a folder of their own."""
    if os.name == "nt":
        return [rf"\\.\pipe\discord-ipc-{number}" for number in range(10)]
    folders = dict.fromkeys(os.environ.get(name) or "/tmp" for name in ("XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"))
    return [os.path.join(folder, inside, f"discord-ipc-{number}") for number in range(10) for folder in folders
            for inside in ("", "snap.discord", "app/com.discordapp.Discord", ".flatpak/com.discordapp.Discord/xdg-run")]


def connect():
    """A stream to the Discord app; OSError when none runs."""
    for address in addresses():
        try:
            if os.name == "nt":
                return open(address, "r+b", buffering=0)
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(5)
                client.connect(address)
                return client.makefile("rwb", buffering=0)  # the socket stays open until this stream closes
        except OSError:
            continue
    raise OSError("Discord is not running")


class Presence:
    """The activity Discord shows for the application `client_id` (its name is what the profile says is played)."""

    def __init__(self, client_id: str, connect=connect, pid: int | None = None) -> None:
        self.client_id, self.connect, self.pid = client_id, connect, pid or os.getpid()
        self.changed = threading.Condition()
        self.since: float | None = None  # when the game started; None: nothing to show
        self.stream = None
        threading.Thread(target=self.run, daemon=True).start()

    def show(self, since: float | None) -> None:
        with self.changed:
            self.since = since
            self.changed.notify()

    def run(self) -> None:
        shown = None
        while True:
            with self.changed:
                # A change, or every RETRY seconds while the game runs: a Discord started or restarted since gets it.
                self.changed.wait_for(lambda: self.since != shown, RETRY if self.since else None)
                shown = self.since
            try:
                if shown:
                    if not self.stream:
                        self.stream = self.connect()
                        self.send(HANDSHAKE, {"v": 1, "client_id": self.client_id})
                    self.activity({"timestamps": {"start": int(shown)}})
                elif self.stream:
                    self.activity(None)
                    self.close()
            except (OSError, ValueError, struct.error):
                self.close()

    def activity(self, activity: dict | None) -> None:
        """Show `activity` for this process (Discord clears it when the process ends), or clear it with None."""
        self.send(FRAME, {"cmd": "SET_ACTIVITY", "args": {"pid": self.pid, **({"activity": activity} if activity else {})},
                          "nonce": uuid.uuid4().hex})

    def send(self, opcode: int, message: dict) -> dict:
        """Send one message and return Discord's answer; ValueError when Discord refuses it (an unknown application)."""
        self.stream.write(frame(opcode, message))
        while True:
            opcode, answer = read_frame(self.stream)
            if opcode == PING:
                self.stream.write(frame(PONG, answer))
            elif opcode == CLOSE or answer.get("evt") == "ERROR":
                raise ValueError(answer.get("message") or (answer.get("data") or {}).get("message", "refused"))
            else:
                return answer

    def close(self) -> None:
        if self.stream:
            try:
                self.stream.close()
            except OSError:
                pass
            self.stream = None
