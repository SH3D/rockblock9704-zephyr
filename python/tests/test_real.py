# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""
A real RockBLOCK 9704, with --real-modem PORT. Free checks only: identity
and signal, never a message, so nothing here costs airtime. Indoors the
signal is usually 0, which passes.
"""

import re

import pytest

from conftest import NativeConsole


@pytest.fixture
def real(request):
    port = request.config.getoption("--real-modem")
    if not port:
        pytest.skip("no --real-modem")
    return port


def test_library_identity(real):
    rb = pytest.importorskip("rockblock9704")
    m = rb.RockBlock9704()
    assert m.begin(real)
    try:
        assert re.fullmatch(r"\d{15}", m.get_imei() or "")
        assert m.get_card_present()
        assert 0 <= m.get_signal() <= 5
    finally:
        m.end()


def test_zephyr_native_sim_identity(real, request):
    exe = request.config.getoption("--zephyr-exe")
    if not exe:
        pytest.skip("no --zephyr-exe")
    c = NativeConsole(exe, real)
    try:
        c.sh("rb9704 begin")
        c.out.expect(r"begin: ok", timeout=20)
        c.sh("rb9704 info")
        c.out.expect(r"imei\s+\d{15}", timeout=20)
        c.sh("rb9704 signal")
        c.out.expect(r"signal: [0-5] bars", timeout=20)
        c.sh("rb9704 end")
        c.out.expect(r"end: ok")
    finally:
        c.close()
