# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""The rb9704 command line against the fake modem."""

import subprocess
import sys

import pytest


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
