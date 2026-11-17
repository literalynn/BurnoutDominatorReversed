import os
import queue
import socket
import struct
import sys
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import discord_presence  # noqa: E402


class FakeDiscord:
    """The Discord app's end of the IPC: it records the frames it gets and answers each as Discord does."""

    def __init__(self) -> None:
        self.received: queue.Queue = queue.Queue()
        self.connections = 0

    def connect(self):
        ours, theirs = socket.socketpair()
        self.connections += 1
        threading.Thread(target=self.serve, args=(theirs,), daemon=True).start()
        with ours:
            return ours.makefile("rwb", buffering=0)

    def serve(self, end: socket.socket) -> None:
        with end, end.makefile("rwb", buffering=0) as stream:
            try:
                while True:
                    opcode, message = discord_presence.read_frame(stream)
                    self.received.put((opcode, message))
                    if opcode == discord_presence.HANDSHAKE:
                        answer = {"cmd": "DISPATCH", "evt": "READY", "data": {"v": 1}}
                    else:
                        answer = {"cmd": message["cmd"], "data": message["args"].get("activity"), "evt": None, "nonce": message["nonce"]}
                    stream.write(discord_presence.frame(discord_presence.FRAME, answer))
            except OSError:
                self.received.put(("closed", None))

    def next(self):
        return self.received.get(timeout=10)


class DiscordPresenceTests(unittest.TestCase):
    def test_frames_are_a_little_endian_opcode_and_length_then_json(self):
        self.assertEqual(discord_presence.frame(1, {"a": 1}), struct.pack("<II", 1, 8) + b'{"a": 1}')
        ours, theirs = socket.socketpair()
        with ours, theirs, theirs.makefile("rb", buffering=0) as stream:
            ours.sendall(discord_presence.frame(3, {"x": "é"}))
            self.assertEqual(discord_presence.read_frame(stream), (3, {"x": "é"}))

    def test_the_game_shows_while_it_runs_and_nothing_else_is_sent(self):
        discord = FakeDiscord()
        presence = discord_presence.Presence("1234", connect=discord.connect, pid=4321)
        presence.show(1_800_000_000.7)
        self.assertEqual(discord.next(), (discord_presence.HANDSHAKE, {"v": 1, "client_id": "1234"}))
        opcode, message = discord.next()
        self.assertEqual((opcode, message["cmd"]), (discord_presence.FRAME, "SET_ACTIVITY"))
        self.assertEqual(message["args"], {"pid": 4321, "activity": {"timestamps": {"start": 1_800_000_000}}})
        presence.show(None)  # the game ended: the activity is cleared and the connection closed
        self.assertEqual(discord.next()[1]["args"], {"pid": 4321})
        self.assertEqual(discord.next(), ("closed", None))

    def test_without_discord_it_carries_on_and_tries_again_later(self):
        discord = FakeDiscord()
        attempts = []

        def connect():
            attempts.append(1)
            if len(attempts) == 1:
                raise OSError("Discord is not running")
            return discord.connect()

        with mock.patch.object(discord_presence, "RETRY", 0.05):
            presence = discord_presence.Presence("1234", connect=connect)
            presence.show(1_800_000_000)  # returns at once: a thread of its own sends it
            self.assertEqual(discord.next()[0], discord_presence.HANDSHAKE)
            self.assertEqual(discord.next()[1]["args"]["pid"], os.getpid())
        self.assertEqual(len(attempts), 2)

    def test_an_unknown_application_is_refused_without_an_exception(self):
        ours, theirs = socket.socketpair()
        with theirs, theirs.makefile("rwb", buffering=0) as stream:
            with mock.patch.object(discord_presence, "RETRY", 3600):
                presence = discord_presence.Presence("0", connect=lambda: ours.makefile("rwb", buffering=0))
                presence.show(1_800_000_000)
                self.assertEqual(discord_presence.read_frame(stream)[0], discord_presence.HANDSHAKE)
                stream.write(discord_presence.frame(discord_presence.CLOSE, {"code": 4000, "message": "Invalid Client ID"}))
                ours.close()
                self.assertEqual(stream.read(1), b"")  # the presence closed its end: it tries again in RETRY seconds

    def test_discord_is_looked_for_where_each_system_puts_it(self):
        if os.name == "nt":
            self.assertEqual(discord_presence.addresses()[:2], [r"\\.\pipe\discord-ipc-0", r"\\.\pipe\discord-ipc-1"])
        else:
            with mock.patch.dict(os.environ, {"XDG_RUNTIME_DIR": "/run/user/1000"}):
                found = discord_presence.addresses()
            self.assertEqual(found[0], "/run/user/1000/discord-ipc-0")
            self.assertIn("/run/user/1000/app/com.discordapp.Discord/discord-ipc-0", found)
            self.assertIn("/tmp/snap.discord/discord-ipc-9", found)


if __name__ == "__main__":
    unittest.main()
