# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""The rb9704 command line against the fake modem."""

import base64
import subprocess
import sys

import pytest

from rb9704tools import pattern


def run(*args, timeout=30):
    return subprocess.run([sys.executable, "-m", "rb9704tools.cli", *args], capture_output=True,
                          text=True, timeout=timeout)


def test_jspr_raw(fake):
    r = run("-p", fake.port, "jspr", "--setup", "GET hwInfo {}")
    assert r.returncode == 0, r.stderr
    assert "300434030000000" in r.stdout


def test_provision_sees_topics(fake):
    r = run("-p", fake.port, "provision", "--timeout", "10")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "244" in r.stdout


def test_info_through_the_library(fake):
    pytest.importorskip("rockblock9704")
    r = run("-p", fake.port, "info")
    assert r.returncode == 0, r.stderr
    assert "300434030000000" in r.stdout


def test_send_asks_first(fake):
    pytest.importorskip("rockblock9704")
    r = subprocess.run([sys.executable, "-m", "rb9704tools.cli", "-p", fake.port, "send",
                        "--text", "x"], input="n\n", capture_output=True, text=True, timeout=30)
    assert "airtime" in (r.stdout + r.stderr).lower()
    assert not any(l.startswith("MO ") for l in fake.out.seen)


def test_send_with_yes(fake):
    pytest.importorskip("rockblock9704")
    r = run("-p", fake.port, "send", "--text", "cli-hello", "--yes")
    assert r.returncode == 0, r.stdout + r.stderr
    m = fake.out.expect(r"^MO 244 \d+ (\w+)")
    assert bytes.fromhex(m.group(1)) == b"cli-hello"


def test_pattern_make_and_check():
    r = run("pattern", "make", "--size", "1000", "--seq", "2", "--count", "3")
    assert r.returncode == 0, r.stderr
    hexed = r.stdout.strip()
    data = bytes.fromhex(hexed)
    assert len(data) == 1000 and data[:4] == b"RBT\x01"
    # the ramp turns at 255 and comes back down
    assert data[255] == 255 and data[256] == 254 and data[510] == 0
    r = run("pattern", "check", "--hex", hexed)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "M 2/3, 1000 bytes" in r.stdout and "every byte as expected" in r.stdout
    b64 = base64.b64encode(data).decode().rstrip("=")
    assert run("pattern", "check", "--b64", b64).returncode == 0


def test_pattern_check_finds_the_damage():
    data = bytearray(pattern.make(100))
    data[40] ^= 1
    r = run("pattern", "check", "--hex", data.hex())
    assert r.returncode != 0 and "byte 40" in r.stdout + r.stderr
    r = run("pattern", "check", "--hex", pattern.make(100)[:90].hex())
    assert r.returncode != 0 and "says 100 bytes, 90 arrived" in r.stdout + r.stderr
    r = run("pattern", "check", "--hex", b"hello".hex())
    assert r.returncode != 0 and "not a test message" in r.stdout + r.stderr
