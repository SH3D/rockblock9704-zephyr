"""
The test harness: a fake modem in a process of its own, and a console to
drive whatever is under test.

    pytest                                    the fake and the CLI, and the
                                              library's Python binding if
                                              rockblock9704 is installed
    pytest --zephyr-exe build/zephyr/zephyr.exe
                                              ... and Zephyr on native_sim
    pytest --board-console /dev/ttyACM0 --board-modem /dev/ttyUSB0
                                              ... and Zephyr on a board: its
                                              shell on one port, its modem
                                              UART wired to a USB-serial
                                              adapter on the other, where the
                                              fake modem answers
    pytest --real-modem /dev/ttyUSB0 [--zephyr-exe ...]
                                              ... and the free checks against
                                              a real modem: nothing is sent

The native_sim and board suites are the same tests (test_zephyr.py) run
through a different console.
"""

import os
import queue
import re
import subprocess
import sys
import threading
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def pytest_addoption(parser):
    g = parser.getgroup("rb9704")
    g.addoption("--zephyr-exe", default=os.environ.get("RB9704_ZEPHYR_EXE"),
                help="the shell sample built for native_sim (zephyr.exe)")
    g.addoption("--board-console", default=os.environ.get("RB9704_BOARD_CONSOLE"),
                help="a board's shell, with the shell sample flashed")
    g.addoption("--board-modem", default=os.environ.get("RB9704_BOARD_MODEM"),
                help="the host serial port wired to the board's modem UART")
    g.addoption("--board-baud", type=int, default=115200)
    g.addoption("--real-modem", default=os.environ.get("RB9704_REAL_MODEM"),
                help="a real RockBLOCK 9704's port, for the free checks in test_real.py")


class Lines:
    """Lines from a stream, collected on a thread, waited on by regex."""

    def __init__(self, stream):
        self.q = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, args=(stream,), daemon=True).start()

    def _read(self, stream):
        for line in stream:
            self.q.put(ANSI.sub("", line).rstrip("\r\n"))

    def expect(self, pattern, timeout=10.0):
        rx = re.compile(pattern)
        end = time.monotonic() + timeout
        while True:
            for line in self.seen:
                m = rx.search(line)
                if m:
                    self.seen = self.seen[self.seen.index(line) + 1:]
                    return m
            left = end - time.monotonic()
            if left <= 0:
                raise AssertionError("no line matching %r in %.0f s; last lines:\n%s"
                                     % (pattern, timeout, "\n".join(self.seen[-15:])))
            try:
                self.seen.append(self.q.get(timeout=min(left, 0.2)))
            except queue.Empty:
                pass

    def clear(self):
        while not self.q.empty():
            self.seen.append(self.q.get())
        self.seen = []


class Fake:
    """rb9704-fake in a subprocess, on a pty or on a real port."""

    def __init__(self, port=None, topics="244"):
        args = [sys.executable, "-m", "rb9704tools.fakemodem", "--quiet", "--topics", topics]
        if port:
            args += ["--port", port]
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1,
                                  cwd=os.path.dirname(HERE))
        self.out = Lines(self.p.stdout)
        self.port = self.out.expect(r"^READY (\S+)").group(1)

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def close(self):
        try:
            self.cmd("quit")
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


@pytest.fixture
def fake():
    f = Fake()
    yield f
    f.close()


class NativeConsole:
    """The shell sample on native_sim, its modem UART on the fake's pty."""

    def __init__(self, exe, modem_port):
        self.p = subprocess.Popen([exe, "-uart_stdinout", "-modem_uart_port=" + modem_port],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.out = Lines(self.p.stdout)
        # Up when the shell answers; the boot log line can come before the
        # shell's log backend is there to print it.
        for _ in range(20):
            self.sh("rb9704 status")
            try:
                self.out.expect(r"powered", timeout=0.5)
                return
            except AssertionError:
                pass
        self.p.kill()
        raise AssertionError("native_sim's shell did not answer")

    def sh(self, line):
        self.out.clear()
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def close(self):
        self.p.kill()


class SerialConsole:
    """The shell sample on a board, over its console port."""

    def __init__(self, port, baud):
        import serial
        self.s = serial.Serial(port, baud, timeout=0.1, write_timeout=2)
        self.out = Lines(self._lines())
        self.sh("")
        self.sh("rb9704 end")  # a clean start whatever the board was doing

    def _lines(self):
        buf = b""
        while True:
            try:
                buf += self.s.read(512)
            except Exception:
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                yield line.decode(errors="replace")

    def sh(self, line):
        self.out.clear()
        data = (line + "\r").encode()
        # A shell's receive buffer is small; typed in one burst a long line
        # loses characters. A person types slower than this.
        for i in range(0, len(data), 16):
            self.s.write(data[i:i + 16])
            time.sleep(0.01)
        time.sleep(0.2)

    def close(self):
        self.s.close()


def _targets(config):
    t = []
    if config.getoption("--zephyr-exe"):
        t.append("native_sim")
    if config.getoption("--board-console"):
        t.append("board")
    return t


def pytest_generate_tests(metafunc):
    if "zephyr" in metafunc.fixturenames:
        targets = _targets(metafunc.config)
        metafunc.parametrize("zephyr", targets or ["none"], indirect=True)


@pytest.fixture
def zephyr(request):
    """(console, fake): Zephyr under test and the fake modem it talks to."""
    cfg = request.config
    if request.param == "none":
        pytest.skip("no Zephyr target: --zephyr-exe or --board-console")
    topics = getattr(request, "node").get_closest_marker("topics")
    topics = topics.args[0] if topics else "244"
    if request.param == "board" and not cfg.getoption("--board-modem"):
        pytest.skip("--board-console needs --board-modem")
    f = Fake(port=cfg.getoption("--board-modem") if request.param == "board" else None,
             topics=topics)
    try:
        if request.param == "native_sim":
            c = NativeConsole(cfg.getoption("--zephyr-exe"), f.port)
        else:
            c = SerialConsole(cfg.getoption("--board-console"), cfg.getoption("--board-baud"))
    except Exception:
        f.close()
        raise
    yield c, f
    c.close()
    f.close()


def pytest_configure(config):
    config.addinivalue_line("markers", "topics(ids): the fake modem's provisioning")
