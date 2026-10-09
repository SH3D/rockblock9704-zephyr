# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""
Ground Control's own Python binding (the rockblock9704 package, the same C
library this repository builds into Zephyr) against the fake modem on a pty.
The fake runs in a process of its own: the binding holds the GIL through
every blocking call.
"""

import time

import pytest

rb = pytest.importorskip("rockblock9704")


@pytest.fixture
def modem(fake):
    m = rb.RockBlock9704()
    assert m.begin(fake.port)
    yield m
    m.end()


def poll_until(m, cond, secs=10):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        m.poll()
        if cond():
            return True
        time.sleep(0.01)
    return False


def test_identity(modem):
    assert modem.get_imei() == "300434030000000"
    assert modem.get_iccid() == "8988169712345678901"
    assert modem.get_firmware_version() == "v1.2.3"
    assert modem.get_signal() == 5


def test_send_async(modem, fake):
    done = []
    modem.set_mo_message_complete_callback(lambda mid, status: done.append(status))
    assert modem.send_message_async(b"from-python", 244)
    assert poll_until(modem, lambda: done)
    assert done == [1]
    m = fake.out.expect(r"^MO 244 \d+ (\w+)")
    assert bytes.fromhex(m.group(1)) == b"from-python"


def test_receive_async(modem, fake):
    got = []
    modem.set_mt_message_complete_callback(lambda mid, status: got.append(status))
    fake.cmd("mt 244 to-python")
    assert poll_until(modem, lambda: got)
    assert modem.receive_message_async() == b"to-python"
    assert modem.acknowledge_receive_head_async()
