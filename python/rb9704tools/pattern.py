# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""
The test message `rbtest` in the shell sample sends, and checks when one
arrives as an MT (samples/shell/src/rbtest.c has the same description).

A 16-byte header:

    0..2   b"RBT"
    3      version, 1
    4      kind: b"S" small, b"L" large, anything else for one made here
    5      seq, from 1
    6      count in the run
    7      0
    8..11  the whole message's length, big-endian
    12..15 uptime in ms when it was made, big-endian

then from byte 16 to the end, byte k is k % 510 counted up 0..255 and back
down 254..1.
"""
import struct

HEADER = 16
MAGIC = b"RBT\x01"


def ramp(k):
    v = k % 510
    return v if v < 256 else 510 - v


def make(length, kind="M", seq=1, count=1, uptime_ms=0):
    if length <= HEADER:
        raise ValueError(f"a test message is more than {HEADER} bytes")
    head = MAGIC + struct.pack(">cBBxII", kind.encode()[:1], seq, count, length, uptime_ms)
    return head + bytes(ramp(k) for k in range(HEADER, length))


def check(data):
    """A dict describing data, with "ok" True when every byte is as made,
    or None when it is not a test message at all."""
    if len(data) < HEADER or not data.startswith(MAGIC):
        return None
    kind, seq, count, length, uptime_ms = struct.unpack(">cBBxII", data[4:HEADER])
    r = {"kind": kind.decode("ascii", "replace"), "seq": seq, "count": count,
         "length": length, "got": len(data), "uptime_ms": uptime_ms, "bad_at": None}
    if length != len(data):
        r["bad_at"] = 8
    else:
        r["bad_at"] = next((k for k in range(HEADER, len(data)) if data[k] != ramp(k)), None)
    r["ok"] = r["bad_at"] is None
    return r
