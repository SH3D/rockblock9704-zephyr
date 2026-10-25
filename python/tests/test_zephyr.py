# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""
Zephyr, through the shell sample, against the fake modem: on native_sim
with --zephyr-exe, on a board with --board-console and --board-modem.
"""

import time
import zlib

import pytest


def begin(z):
    c, f = z
    c.sh("rb9704 begin")
    c.out.expect(r"begin: ok")


def test_begin_and_info(zephyr):
    c, f = zephyr
    begin(zephyr)
    c.sh("rb9704 info")
    c.out.expect(r"imei\s+300434030000000")
    c.out.expect(r"firmware\s+v1\.2\.3")
    c.out.expect(r"sim\s+card present, connected")


def test_begin_twice_is_already(zephyr):
    c, f = zephyr
    begin(zephyr)
    c.sh("rb9704 begin")
    c.out.expect(r"begin: -\d+ EALREADY")


def test_signal(zephyr):
    c, f = zephyr
    begin(zephyr)
    f.cmd("signal 3")
    c.out.expect(r"signal 3 bars")
    c.sh("rb9704 signal")
    c.out.expect(r"signal: 3 bars")


def test_send_reaches_the_network(zephyr):
    c, f = zephyr
    begin(zephyr)
    c.sh("rb9704 send 244 hello-network")
    c.out.expect(r"send: ok")
    m = f.out.expect(r"^MO 244 (\d+) (\w+)")
    assert bytes.fromhex(m.group(2)) == b"hello-network"
    c.out.expect(r"mo done topic 244 id %s result 0" % m.group(1))


def test_send_in_many_segments(zephyr):
    c, f = zephyr
    begin(zephyr)
    f.cmd("segment 16")
    text = "".join(chr(ord("a") + i % 26) for i in range(150))
    c.sh("rb9704 send 244 " + text)
    m = f.out.expect(r"^MO 244 \d+ (\w+)")
    assert bytes.fromhex(m.group(1)).decode() == text
    c.out.expect(r"result 0")


def test_send_failure_reaches_the_callback(zephyr):
    c, f = zephyr
    begin(zephyr)
    f.cmd("mo-fail network_error")
    c.sh("rb9704 send 244 doomed")
    c.out.expect(r"mo done topic 244 id \d+ result -\d+")


def test_send_refused_by_the_modem(zephyr):
    c, f = zephyr
    begin(zephyr)
    f.cmd("mo-refuse")
    c.sh("rb9704 send 244 refused")
    c.out.expect(r"send: -\d+ EIO")


def test_cancel_a_send_the_modem_sits_on(zephyr):
    """The modem took the message and said nothing more, as one waiting for
    a satellite does; cancel frees the module, and a send then goes."""
    c, f = zephyr
    begin(zephyr)
    f.cmd("mo-hold")
    c.sh("rb9704 send 244 stuck")
    c.out.expect(r"send: ok")
    m = f.out.expect(r"^MO 244 (\d+) ")
    c.sh("rb9704 info")
    c.out.expect(r"info: -\d+ EBUSY")
    c.sh("rb9704 status")
    c.out.expect(r"one in flight: topic 244 id %s" % m.group(1))
    c.sh("rb9704 cancel")
    c.out.expect(r"cancel: ok")
    c.out.expect(r"mo done topic 244 id %s result -\d+" % m.group(1))
    c.sh("rb9704 cancel")
    c.out.expect(r"cancel: -\d+ ENOENT")
    c.sh("rb9704 send 244 after-cancel")
    c.out.expect(r"send: ok")
    f.out.expect(r"^MO 244 \d+ ")
    c.out.expect(r"result 0")


def test_a_boot_burst_while_closed(zephyr):
    """A modem powered up before begin talks to nobody: its boot lines,
    over 600 bytes here, end part-way through one, finished just before
    the first reply. On the M5Stamp-C3 such a burst overflowed the UART's
    128-byte FIFO while the module was not listening, and begin then heard
    only repeated fragments of old lines; given one, the library read past
    its buffer and the board crashed (2026-10-25). The module now empties
    the FIFO from boot and skips the rest of a line it opened in.

    On a board through the rig this is that overflow. native_sim has no
    FIFO to overflow: there it checks that the burst does no harm."""
    c, f = zephyr
    boot = ('\\x11\\x11299 bootInfo {"image_type":"production","version":{"major":1,'
            '"minor":5,"patch":14,"build_info":"SX-250613-A3E9C3E3-P1-P3142-1194848-'
            '12516979"},"boot_source":"primary"}\\r')
    lines = (boot
             + '299 simStatus {"card_present":true,"sim_connected":true,'
               '"iccid":"8988169712345678901"}\\r'
             + '299 operationalState {"state":"inactive","reason":0}\\r' * 4
             + '299 constellationState {"constellation_visible":false,"signal_bars":0}\\r' * 2
             + '299 bootInfo {"image_type":"prod')
    f.cmd("raw " + lines)
    f.out.expect(r"^RAW ")
    time.sleep(0.5)
    f.cmd('before uction"}\\r')
    f.out.expect(r"^BEFORE ")
    begin(zephyr)
    c.sh("rb9704 info")
    c.out.expect(r"imei\s+300434030000000")


def test_topic_below_64_is_refused_here(zephyr):
    c, f = zephyr
    begin(zephyr)
    c.sh("rb9704 send 10 nope")
    c.out.expect(r"send: -\d+ EINVAL")


@pytest.mark.topics("")
def test_provisioning_that_arrives_later_is_seen(zephyr):
    """The library keeps the first answer for good; the module asks again."""
    c, f = zephyr
    begin(zephyr)
    c.sh("rb9704 send 244 too-early")
    # EACCES when the library asks first; EIO when it had 244 cached from an
    # earlier test on the same boot and the modem refused the message.
    c.out.expect(r"send: -\d+ (EACCES|EIO)")
    f.cmd("topics 244")
    c.sh("rb9704 send 244 now-provisioned")
    c.out.expect(r"send: ok")
    f.out.expect(r"^MO 244 ")


def test_message_from_the_network(zephyr):
    c, f = zephyr
    begin(zephyr)
    f.cmd("mt 244 hello-buoy")
    c.out.expect(r"mt topic 244 id \d+ len 10 crc %08x: hello-buoy$" % zlib.crc32(b"hello-buoy"))
    c.sh("rb9704 status")
    c.out.expect(r"mt\s+1 received")


def test_long_message_from_the_network(zephyr):
    c, f = zephyr
    begin(zephyr)
    body = "".join(chr(ord("a") + i % 26) for i in range(1500))  # two MT segments
    f.cmd("mt 244 " + body)
    c.out.expect(r"mt topic 244 id \d+ len 1500 crc %08x: %s$"
                 % (zlib.crc32(body.encode()), body[:64]))


def test_end(zephyr):
    c, f = zephyr
    begin(zephyr)
    c.sh("rb9704 end")
    c.out.expect(r"end: ok")
    c.sh("rb9704 send 244 after-end")
    c.out.expect(r"send: -\d+ ENOTCONN")
