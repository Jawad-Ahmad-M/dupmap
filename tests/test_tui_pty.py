#!/usr/bin/env python3
"""Exercise TUI startup/help/quit under several terminal dimensions (POSIX)."""
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time


def run_case(program, fixture, columns, rows, show_help, resize_to=None):
    env = os.environ.copy()
    env["TERM"] = "xterm-256color"
    with tempfile.TemporaryDirectory(prefix="dupmap-ui-state-") as state_dir:
        env["XDG_STATE_HOME"] = state_dir
        pid, master = pty.fork()
        if pid == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
            os.execve(program, [program, fixture], env)
        output = bytearray()
        try:
            deadline = time.monotonic() + 8
            sent = False
            resized = False
            status = None
            while time.monotonic() < deadline:
                ready, _, _ = select.select([master], [], [], 0.1)
                if ready:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        chunk = b""
                    output.extend(chunk)
                waited, child_status = os.waitpid(pid, os.WNOHANG)
                if waited:
                    status = child_status
                    break
                if not sent and len(output) > 0:
                    os.write(master, b"?" if show_help else b"q")
                    sent = True
                if show_help and b"Help is open" in output and resize_to and not resized:
                    before_resize = len(output)
                    new_columns, new_rows = resize_to
                    fcntl.ioctl(master, termios.TIOCSWINSZ,
                                struct.pack("HHHH", new_rows, new_columns, 0, 0))
                    resize_deadline = time.monotonic() + 2
                    while len(output) == before_resize and time.monotonic() < resize_deadline:
                        ready, _, _ = select.select([master], [], [], 0.1)
                        if ready:
                            try:
                                output.extend(os.read(master, 65536))
                            except OSError:
                                break
                    if len(output) == before_resize:
                        raise AssertionError("TUI did not redraw after terminal resize")
                    os.write(master, b"\x1b")
                    time.sleep(1.2)
                    os.write(master, b"l\x1b[B\r\x08sfneedle\nq")
                    resized = True
            if status is None:
                os.kill(pid, signal.SIGKILL)
                _, status = os.waitpid(pid, 0)
                raise AssertionError(f"TUI did not exit at {columns}x{rows}")
            return_code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status)
            if return_code != 0:
                raise AssertionError(
                    f"TUI exited {return_code} at {columns}x{rows}; "
                    f"terminal output: {bytes(output[-2000:])!r}"
                )
        finally:
            try:
                waited, _ = os.waitpid(pid, os.WNOHANG)
                if not waited:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
            except ChildProcessError:
                pass
            os.close(master)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_tui_pty.py DUPMAP FIXTURE")
    program = os.path.abspath(sys.argv[1])
    fixture = os.path.abspath(sys.argv[2])
    run_case(program, fixture, 80, 24, True, resize_to=(47, 11))
    run_case(program, fixture, 47, 11, False)
    run_case(program, fixture, 15, 5, False)
    print("pseudo-terminal UI tests: all passed")


if __name__ == "__main__":
    main()
