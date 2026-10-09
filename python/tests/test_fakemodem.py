"""The fake modem on its own, in this process: no serial port, no library."""

import base64
import json

from rb9704tools.fakemodem import FakeModem, crc16_xmodem, with_crc


class Wire:
    def __init__(self, topics=(244,)):
        self.out = []
        self.events = []
        self.m = FakeModem(self.out.append, self.events.append, topics)

    def put(self, line):
        self.m.feed((line + "\r").encode())

    def lines(self):
        got = [b.decode().rstrip("\r") for b in self.out]
        self.out.clear()
        return [(int(l.split(" ", 2)[0]), l.split(" ", 2)[1], json.loads(l.split(" ", 2)[2]))
                for l in got]


def test_crc_is_xmodem():
    assert crc16_xmodem(b"123456789") == 0x31C3
    assert with_crc(b"hello") == bytes.fromhex("68656c6c6fc362")


def test_begin_sequence():
    w = Wire()
    w.put("GET apiVersion {}")
    w.put("GET simConfig {}")
    w.put("GET operationalState {}")
    got = w.lines()
    assert [(c, t) for c, t, _ in got] == [(200, "apiVersion"), (200, "simConfig"),
                                           (200, "operationalState")]
    assert "active_version" in got[0][2]
    assert got[1][2]["interface"] == "internal"
    assert got[2][2]["state"] == "active"


def test_put_sim_config_answers_then_reports_status():
    w = Wire()
    w.put('PUT simConfig {"interface": "internal"}')
    assert [(c, t) for c, t, _ in w.lines()] == [(200, "simConfig"), (299, "simStatus")]


def test_mo_in_segments_with_crc():
    w = Wire()
    w.m.segment = 4
    payload = b"hello world"
    whole = with_crc(payload)
    w.put('PUT messageOriginate {"topic_id":244, "message_length":%d, "request_reference":1}'
          % len(whole))
    got = w.lines()
    assert got[0][:2] == (200, "messageOriginate")
    mid = got[0][2]["message_id"]
    asks = [got[1]]
    while True:
        code, target, body = asks[-1]
        if target == "messageOriginateStatus":
            break
        assert (code, target) == (299, "messageOriginateSegment")
        s, n = body["segment_start"], body["segment_length"]
        w.put('PUT messageOriginateSegment {"topic_id":244, "message_id":%d, "segment_length":%d,'
              ' "segment_start":%d, "data":"%s"}'
              % (mid, n, s, base64.b64encode(whole[s:s + n]).decode()))
        asks = [l for l in w.lines() if l[0] == 299]
    assert asks[-1][2]["final_mo_status"] == "mo_ack_received"
    assert "MO 244 %d %s" % (mid, payload.hex()) in w.events


def test_mo_with_a_bad_crc():
    w = Wire()
    bad = b"abc\x00\x00"
    w.put('PUT messageOriginate {"topic_id":244, "message_length":5, "request_reference":1}')
    mid = w.lines()[0][2]["message_id"]
    w.put('PUT messageOriginateSegment {"topic_id":244, "message_id":%d, "segment_length":5,'
          ' "segment_start":0, "data":"%s"}' % (mid, base64.b64encode(bad).decode()))
    status = [b for c, t, b in w.lines() if t == "messageOriginateStatus"]
    assert status[0]["final_mo_status"] == "crc_error_in_transfer"
    assert any(e.startswith("MOBAD 244") for e in w.events)


def test_unprovisioned_topic_is_refused():
    w = Wire(topics=())
    w.put('PUT messageOriginate {"topic_id":244, "message_length":7, "request_reference":1}')
    assert w.lines()[0][:2] == (406, "messageOriginate")


def test_mt_segments_fit_the_library():
    w = Wire()
    w.m.send_mt(244, b"y" * 3000)
    got = w.lines()
    segs = [b for c, t, b in got if t == "messageTerminateSegment"]
    assert all(len(s["data"]) <= 1446 for s in segs)
    data = b"".join(base64.b64decode(s["data"]) for s in segs)
    assert data == with_crc(b"y" * 3000)
    assert got[-1][2]["final_mt_status"] == "complete"


def test_unknown_target_is_404():
    w = Wire()
    w.put("GET nonsense {}")
    assert w.lines()[0][:2] == (404, "nonsense")
