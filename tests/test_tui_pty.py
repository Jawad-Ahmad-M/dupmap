#!/usr/bin/env python3
"""Check rendered ncurses screens and real keyboard navigation under a POSIX PTY."""
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import struct
import tempfile
import termios
import time
import sys

from terminal_screen import Screen

HOME, END, DOWN, PAGE_DOWN = b"\x1bOH", b"\x1bOF", b"\x1bOB", b"\x1b[6~"


class Session:
    def __init__(self, program, fixture, columns=80, rows=24, terminal="xterm-256color"):
        self.state = tempfile.TemporaryDirectory(prefix="dupmap-ui-state-")
        self.screen = Screen(columns, rows)
        self.status = None
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            env = os.environ.copy()
            env.update(TERM=terminal, XDG_STATE_HOME=self.state.name, LC_ALL="C.UTF-8")
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
            os.execve(program, [program, str(fixture)], env)

    def __enter__(self): return self

    def __exit__(self, *_):
        if self.status is None:
            waited, status = os.waitpid(self.pid, os.WNOHANG)
            if not waited:
                os.kill(self.pid, signal.SIGKILL)
                _, status = os.waitpid(self.pid, 0)
            self.status = status
        os.close(self.master)
        self.state.cleanup()

    def read(self):
        ready, _, _ = select.select([self.master], [], [], 0.05)
        if ready:
            try: self.screen.feed(os.read(self.master, 65536))
            except OSError: pass
        if self.status is None:
            waited, status = os.waitpid(self.pid, os.WNOHANG)
            if waited: self.status = status

    def wait(self, condition, description):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            self.read()
            if condition(self.screen.text()):
                # ncurses may split one redraw across writes. Do not resize or
                # send another action while a matching frame is still arriving.
                ready, _, _ = select.select([self.master], [], [], 0.05)
                if not ready: return self.screen.text()
            if self.status is not None: break
        raise AssertionError(f"Missing {description}; status={self.status}\n{self.screen.text()}")

    def expect(self, text):
        return self.wait(lambda screen: text in screen, repr(text))

    def send(self, keys): os.write(self.master, keys)

    def resize(self, columns, rows):
        self.screen.resize(columns, rows)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))

    def quit(self):
        self.send(b"q")
        deadline = time.monotonic() + 5
        while self.status is None and time.monotonic() < deadline: self.read()
        assert self.status is not None, "TUI did not exit"
        assert os.waitstatus_to_exitcode(self.status) == 0, self.screen.text()
        assert not self.screen.colored, "Dashboard emitted foreground/background colors"


def check_navigation(program, fixture):
    with Session(program, fixture) as session:
        screen = session.expect("Dashboard 1-6 of 74")
        if os.environ.get("DUPMAP_PREVIEW_PATH"):
            Path(os.environ["DUPMAP_PREVIEW_PATH"]).write_text(screen, encoding="utf-8")
        assert "Files here: 3" in screen and "Duplicates: not checked" in screen
        assert "Total: not calculated" not in screen
        assert "folder-000" in screen and "root-only.txt" not in screen
        assert "+------------------------+" in screen
        session.send(END)
        session.expect("资料文件夹")
        session.wait(lambda screen: "More below" not in screen, "last folder page")
        session.send(HOME)
        session.expect("Dashboard 1-6 of 74")

        session.send(b"3")
        screen = session.expect("Files 1-3 of 3")
        assert "root-only.txt" in screen and "other" in screen and "folder-000" not in screen
        session.send(b"froot-only\n")
        session.expect("Files 1-1 of 1")
        session.send(b"2")
        session.expect("Folders 1-")
        session.send(b"3")
        session.expect("Files 1-1 of 1")
        session.send(b"f\x15missing")
        session.expect("No items match")
        session.send(b"\x1b")
        session.expect("Files 1-1 of 1")
        session.send(b"s")
        session.expect("Sort: name")
        assert "root-only.txt" in session.screen.text()

        session.send(b"d")
        session.expect("Duplicates 1-1 of 1")
        session.send(b"\r")
        screen = session.expect("Duplicates 1-18 of 71")
        assert "[-] Group 1" in screen and "folder-000/shared.bin" in screen
        session.send(END)
        session.expect("folder-069/shared.bin")
        session.send(b"\r")
        session.expect("Duplicates 1-1 of 1")
        session.send(b"\x1b")
        session.expect("Files 1-1 of 1")

        session.send(b"2ffolder-000\n")
        session.expect("Folders 1-1 of 1")
        session.send(b"\r")
        session.expect("No folders here")
        session.send(b"3")
        session.expect("shared.bin")
        session.send(b"\x7f")
        screen = session.expect("Files 1-3 of 3")
        assert "root-only.txt" in screen

        session.send(b"l")
        session.expect("All items 1-")
        session.send(END)
        session.expect("资料文件夹")
        session.send(b"1")
        session.expect("Dashboard 1-6 of 74")
        session.send(PAGE_DOWN)
        session.expect("Dashboard 3-8 of 74")
        session.send(b"?")
        session.expect("Help is open")
        session.resize(47, 11)
        session.expect("Help is open")
        session.send(b"\x1b")
        session.expect("Dashboard")
        session.send(HOME)
        session.expect("café-folder")
        session.resize(27, 10)
        session.expect("+------------------------+")
        session.resize(25, 10)
        screen = session.expect("[D]")
        assert "+------------------------+" not in screen
        session.send(b"fmissing")
        session.expect("No items match")
        session.resize(15, 5)
        session.expect("Terminal too")
        session.send(b"\x1b")
        session.resize(80, 24)
        session.expect("Dashboard 1-6 of 74")
        session.quit()


def check_lazy_duplicates(program):
    # Change same-size contents after startup to prove the first D reads them
    # at request time. Change them again to prove repeated D reuses the cache,
    # including a completed scan that found no groups.
    for matches_on_request in (True, False):
        with tempfile.TemporaryDirectory(prefix="dupmap-lazy-") as temporary:
            fixture = Path(temporary)
            (fixture / "a").write_bytes(b"aaaa")
            second = fixture / "b"
            second.write_bytes(b"bbbb" if matches_on_request else b"aaaa")
            with Session(program, fixture) as session:
                session.expect("Duplicates: not checked")
                second.write_bytes(b"aaaa" if matches_on_request else b"bbbb")
                session.send(b"d")
                expected = "Duplicates 1-1 of 1" if matches_on_request else "No duplicate files"
                session.expect(expected)
                session.send(b"\x1b")
                session.expect(f"Duplicate groups: {int(matches_on_request)}")
                second.write_bytes(b"bbbb" if matches_on_request else b"aaaa")
                session.send(b"d")
                session.expect(expected)
                session.quit()

    if sys.platform == "linux":
        with tempfile.TemporaryDirectory(prefix="dupmap-lazy-io-") as temporary:
            fixture = Path(temporary)
            size = 8 * 1024 * 1024
            payload = b"x" * size
            (fixture / "a").write_bytes(payload)
            (fixture / "b").write_bytes(payload)
            with Session(program, fixture) as session:
                session.expect("Duplicates: not checked")
                io_path = Path(f"/proc/{session.pid}/io")
                def read_characters():
                    return int(dict(line.split(": ") for line in io_path.read_text().splitlines())["rchar"])
                before = read_characters()
                assert before < size, "Startup read file contents before D"
                session.send(b"d")
                session.expect("Duplicates 1-1 of 1")
                assert read_characters() - before >= 2 * size, "First D did not read duplicate candidates"
                session.send(b"\x1b")
                session.expect("Duplicate groups: 1")
                before = read_characters()
                session.send(b"d")
                session.expect("Duplicates 1-1 of 1")
                assert read_characters() - before < 4096, "Repeated D reread file contents"
                session.quit()


def main():
    if len(sys.argv) != 3: raise SystemExit("usage: test_tui_pty.py DUPMAP FIXTURE")
    program = os.path.abspath(sys.argv[1])
    check_lazy_duplicates(program)
    with tempfile.TemporaryDirectory(prefix="dupmap-dashboard-") as temporary:
        fixture = Path(temporary)
        for i in range(70):
            folder = fixture / f"folder-{i:03d}"
            folder.mkdir()
            (folder / "shared.bin").write_bytes(b"duplicate payload")
        for name in ("café-folder", "资料文件夹", "long-" + "L" * 70):
            folder = fixture / name
            folder.mkdir()
            (folder / "unique.txt").write_text(name)
        (fixture / "empty-folder").mkdir()
        (fixture / "root-only.txt").write_text("root-only contents")
        (fixture / "other").write_text("real file")
        (fixture / "empty.txt").touch()
        check_navigation(program, fixture)
        for columns, rows, terminal in ((80, 24, "vt100"), (47, 11, "xterm-256color"),
                                        (16, 7, "xterm-256color"), (15, 5, "xterm-256color")):
            with Session(program, fixture, columns, rows, terminal) as session:
                session.expect("Terminal too" if columns < 16 else "Dashboard")
                session.quit()
        empty = fixture / "empty-folder"
        with Session(program, empty) as session:
            session.expect("No folders here")
            session.send(b"3")
            session.expect("No files directly")
            session.send(b"d")
            session.expect("No duplicate files")
            session.quit()
        lazy_fixture = fixture / "lazy-navigation"
        lazy_fixture.mkdir()
        nested = lazy_fixture / "nested"
        nested.mkdir()
        (nested / "present-at-start.txt").write_text("new")
        with Session(program, lazy_fixture) as session:
            screen = session.expect("Dashboard 1-1 of 1")
            assert "Total: 3 B" in screen
            (nested / "added-after-start.txt").write_text("new")
            session.send(b"\r")
            screen = session.expect("No folders here")
            assert "Total: 3 B" in screen
            session.send(b"3")
            screen = session.expect("present-at-start.txt")
            assert "added-after-start.txt" not in screen
            session.send(b"\x7f")
            session.expect("Files")
            (nested / "not-in-cache.txt").write_text("later")
            session.send(b"2\r3")
            screen = session.expect("Files 1-1 of 1")
            assert "present-at-start.txt" in screen and "not-in-cache.txt" not in screen
            session.quit()
        sort_fixture = fixture / "sort-check"
        sort_fixture.mkdir()
        for name, size in (("zulu", 20), ("alpha", 10)):
            folder = sort_fixture / name
            folder.mkdir()
            (folder / "payload").write_bytes(b"x" * size)
            os.utime(folder, (size, size))
        with Session(program, sort_fixture) as session:
            session.expect("Dashboard 1-2 of 2")
            session.send(b"2")
            session.wait(lambda screen: screen.splitlines()[-2].strip().endswith("/zulu"),
                         "selected zulu folder")
            session.send(b"d")
            session.expect("No duplicate files")
            session.send(b"\x1b")
            session.wait(lambda screen: screen.splitlines()[2].startswith("Folders") and
                         screen.splitlines()[-2].strip().endswith("/zulu"),
                         "selection preserved after duplicate check")
            screen = session.screen.text()
            assert screen.index("zulu") < screen.index("alpha")
            session.send(b"1ss")
            session.expect("Sort: modified")
            session.send(b"2")
            session.wait(lambda screen: screen.splitlines()[2].startswith("Folders") and
                         screen.splitlines()[-2].strip().endswith("/zulu"),
                         "inactive tab selection preserved after sorting")
            session.quit()
    print("pseudo-terminal dashboard tests: all passed")


if __name__ == "__main__": main()
