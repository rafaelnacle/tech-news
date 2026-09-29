#!/usr/bin/env python3
"""Exercise the real TUI in a PTY without contacting publishers."""
import codecs
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unicodedata


class Terminal:
    def __init__(self, width, height):
        self.resize(width, height)
        self.pending = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")

    def resize(self, width, height):
        self.width, self.height = width, height
        self.cells = [[" "] * width for _ in range(height)]
        self.x = self.y = 0

    def feed(self, data):
        self.pending += self.decoder.decode(data)
        text, i = self.pending, 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                if i + 1 >= len(text):
                    break
                if text[i + 1] == "[":
                    match = re.match(r"\x1b\[([0-9;?<>:]*)([ -/]*)([@-~])", text[i:])
                    if not match:
                        break
                    params, _, final = match.groups()
                    args = [int(p or "0") for p in params.lstrip("?<>").split(";") if ":" not in p]
                    n = args[0] if args and args[0] else 1
                    if final in "Hf":
                        self.y = min(self.height - 1, max(0, n - 1))
                        self.x = min(self.width - 1, max(0, (args[1] if len(args) > 1 and args[1] else 1) - 1))
                    elif final == "A": self.y = max(0, self.y - n)
                    elif final == "B": self.y = min(self.height - 1, self.y + n)
                    elif final == "C": self.x = min(self.width - 1, self.x + n)
                    elif final == "D": self.x = max(0, self.x - n)
                    elif final == "G": self.x = min(self.width - 1, n - 1)
                    elif final == "d": self.y = min(self.height - 1, n - 1)
                    elif final == "J":
                        value = args[0] if args else 0
                        if value in (2, 3): self.cells = [[" "] * self.width for _ in range(self.height)]
                        elif value == 0:
                            self.cells[self.y][self.x:] = [" "] * (self.width - self.x)
                            for row in range(self.y + 1, self.height): self.cells[row] = [" "] * self.width
                    elif final == "K":
                        value = args[0] if args else 0
                        if value == 2: self.cells[self.y] = [" "] * self.width
                        elif value == 0: self.cells[self.y][self.x:] = [" "] * (self.width - self.x)
                        elif value == 1: self.cells[self.y][:self.x + 1] = [" "] * (self.x + 1)
                    elif final == "h" and params == "?1049":
                        self.cells = [[" "] * self.width for _ in range(self.height)]
                        self.x = self.y = 0
                    i += len(match.group(0))
                    continue
                if text[i + 1] in "]P":
                    end = re.search(r"\x07|\x1b\\", text[i + 2:])
                    if not end: break
                    i += 2 + end.end()
                    continue
                i += 2
                continue
            if ch == "\r": self.x = 0
            elif ch == "\n": self.y = min(self.height - 1, self.y + 1)
            elif ch == "\b": self.x = max(0, self.x - 1)
            elif ord(ch) >= 32:
                if self.x >= self.width:
                    self.x = 0
                    self.y = min(self.height - 1, self.y + 1)
                if unicodedata.combining(ch):
                    if self.x: self.cells[self.y][self.x - 1] += ch
                else:
                    self.cells[self.y][self.x] = ch
                    self.x += 2 if unicodedata.east_asian_width(ch) in "WF" else 1
            i += 1
        self.pending = text[i:]

    def text(self):
        return "\n".join("".join(row).rstrip() for row in self.cells)


class Session:
    def __init__(self, binary, directory, width=120, height=30, color=False, offline=True):
        self.master, self.slave = pty.openpty()
        self.original = termios.tcgetattr(self.slave)
        self.terminal = Terminal(width, height)
        self.raw = bytearray()
        self.resize(width, height, notify=False)
        env = dict(os.environ, TERM="xterm-256color", HOME=str(directory), XDG_DATA_HOME=str(directory))
        if color: env.pop("NO_COLOR", None)
        else: env["NO_COLOR"] = "1"
        self.process = subprocess.Popen([str(binary)] + (["--offline"] if offline else []),
                                        stdin=self.slave, stdout=self.slave, stderr=self.slave, env=env)

    def resize(self, width, height, notify=True):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))
        self.terminal.resize(width, height)
        if notify: self.process.send_signal(signal.SIGWINCH)

    def pump(self, duration=0.12):
        end = time.monotonic() + duration
        while time.monotonic() < end:
            if select.select([self.master], [], [], max(0, end - time.monotonic()))[0]:
                data = os.read(self.master, 65536)
                self.raw.extend(data)
                self.terminal.feed(data)
                if b"\x1b[6n" in data: os.write(self.master, b"\x1b[1;1R")

    def expect(self, phrase, timeout=4):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump()
            if phrase in self.terminal.text(): return
            if self.process.poll() is not None: break
        raise AssertionError(f"Missing {phrase!r}:\n{self.terminal.text()}")

    def keys(self, text):
        os.write(self.master, text.encode())
        self.pump()

    def finish(self, control_c=False, terminate=False):
        if terminate: self.process.send_signal(signal.SIGTERM)
        else: self.keys("\x03" if control_c else "q")
        self.process.wait(timeout=8)
        self.pump()
        assert termios.tcgetattr(self.slave) == self.original, "Terminal settings not restored"
        assert b"\x1b[?1049l" in self.raw, "Alternate screen not restored"
        assert self.process.returncode == (128 + signal.SIGTERM if terminate else 0), f"Exit failed: {self.process.returncode}"
        os.close(self.master)
        os.close(self.slave)

    def abort(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        os.close(self.master)
        os.close(self.slave)


def storage(directory):
    base = directory / "Library" / "Application Support" if sys.platform == "darwin" else directory
    return base / "tech-news" / "news.sqlite3"


def seed(directory):
    path = storage(directory)
    path.parent.mkdir(parents=True)
    now = int(time.time())
    with sqlite3.connect(path) as db:
        db.executescript("""
            CREATE TABLE stories(url TEXT PRIMARY KEY,title TEXT NOT NULL,summary TEXT NOT NULL,content TEXT NOT NULL,
              source TEXT NOT NULL,language TEXT NOT NULL,published INTEGER NOT NULL,seen INTEGER NOT NULL,
              kind INTEGER NOT NULL,read INTEGER NOT NULL DEFAULT 0,bookmarked INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL);
            PRAGMA user_version=1;
        """)
        body = "\n\n".join(f"Paragraph {i}: Software tools keep the terminal readable. " * 4 for i in range(1, 25))
        for i, (title, source, language) in enumerate([
                ("First software headline", "Ars Technica", "en"),
                ("Second security headline", "TechCrunch", "en"),
                ("Inteligência artificial no Brasil", "Tecnoblog", "pt-BR")]):
            db.execute("INSERT INTO stories VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                       (f"https://news.example/{i}", title, "Fixture summary: segurança e proteção.", body,
                        source, language, now - i, now, 1, 0, 0))
        db.execute("INSERT INTO metadata VALUES('last_refresh',?)", (now,))


def run(binary):
    assert subprocess.run([binary, "--help"], capture_output=True).returncode == 0
    version = subprocess.run([binary, "--version"], capture_output=True, text=True)
    assert version.returncode == 0 and "tech-news 1.0.0" in version.stdout
    assert subprocess.run([binary, "--invalid"], capture_output=True).returncode == 2
    assert subprocess.run([binary, "--offline"], capture_output=True).returncode == 2
    with tempfile.TemporaryDirectory(prefix="tech-news-pty-") as tmp:
        directory = Path(tmp)
        seed(directory)
        session = Session(binary, directory)
        try:
            session.expect("Sources & filters")
            session.expect("First software headline")
            session.expect("Article preview")
            session.keys("jbu")
            session.expect("Marked read")
            with sqlite3.connect(storage(directory)) as db:
                assert db.execute("SELECT read,bookmarked FROM stories WHERE url='https://news.example/1'").fetchone() == (1, 1)
            session.keys("\t")
            session.expect("> Article preview")
            session.keys("\t")
            session.expect("> Sources & filters")
            session.keys("\t")
            session.expect("> Headlines")
            session.keys("/inteligencia\r")
            session.expect("Headlines (1)")
            session.expect("Inteligência artificial no Brasil")
            session.keys("\x1b")
            session.expect("Headlines (3)")
            session.keys("?")
            session.expect("Keyboard help")
            session.keys("\x1b")
            session.resize(100, 26)
            session.expect("Article preview")
            assert "Sources & filters" not in session.terminal.text(), "Sidebar should collapse"
            session.resize(70, 24)
            session.expect("Headlines (3)")
            assert "Article preview" not in session.terminal.text(), "Narrow view should show one pane"
            session.keys("\r")
            session.expect("> Reader")
            session.expect("Complete feed content")
            initial_reader = session.terminal.text()
            session.keys("\x1b[6~\x1b[6~")
            assert initial_reader != session.terminal.text(), "Reader must scroll"
            session.keys("\x1b")
            session.expect("Headlines (3)")
            session.keys("ll")
            session.expect("Headlines (1)")
            session.expect("Portuguese")
            session.keys("l")
            session.expect("Headlines (3)")
            session.keys("m")
            session.expect("Headlines (1)")
            session.expect("Second security headline")
            session.keys("m")
            session.resize(50, 15)
            session.expect("Resize terminal")
            session.resize(120, 30)
            session.expect("Sources & filters")
            session.resize(120, 18)
            session.expect("Sources & filters")
            session.keys("\x1b[Z" + "j" * 9)
            session.expect("Reset filters")
            session.keys("\r\t")
            session.expect("Headlines (3)")
            assert not re.search(rb"\x1b\[[0-9;]*(?:38|48);[25];", session.raw), "NO_COLOR emitted color sequences"
            session.finish()
        except BaseException:
            session.abort()
            raise
        session = Session(binary, directory)
        try:
            session.expect("Headlines (3)")
            session.finish(terminate=True)
        except BaseException:
            session.abort()
            raise
        # Restart, bookmark-only filter, color palette, and Ctrl-C restoration.
        session = Session(binary, directory, color=True)
        try:
            session.expect("Headlines (3)")
            session.keys("m")
            session.expect("Second security headline")
            session.expect("Headlines (1)")
            assert re.search(rb"\x1b\[[0-9;]*(?:38|48);[25];", session.raw), "Color palette missing"
            session.finish(control_c=True)
        except BaseException:
            session.abort()
            raise
    with tempfile.TemporaryDirectory(prefix="tech-news-empty-") as tmp:
        session = Session(binary, Path(tmp), width=70, height=22)
        try:
            session.expect("No cached stories")
            session.keys("r")
            session.expect("restart without --offline")
            session.finish()
        except BaseException:
            session.abort()
            raise
    print("PASS CLI, offline startup, navigation, flags/restart, search, filters, reader scrolling, resize, NO_COLOR, Ctrl-C/quit/SIGTERM restoration")


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
