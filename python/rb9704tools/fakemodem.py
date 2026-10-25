# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""
rb9704-fake - a fake RockBLOCK 9704: JSPR over a serial port, no satellites.

    rb9704-fake                      make a pty, print its path, serve it
    rb9704-fake --port /dev/ttyUSB0  serve a real serial port instead (a
                                     USB-serial adapter wired to a board's
                                     modem pins)

It answers what Ground Control's library asks (docs/JSPR.md is the
protocol, taken from the library's source), takes MO messages segment by
segment and checks their CRC, and sends MT messages, signal and
provisioning when told to. One fake serves three kinds of test: the
library's own Python binding on the host, Zephyr on native_sim, and Zephyr
on a board through a USB-serial adapter.

Control is one command per line on stdin, events one per line on stdout:

    mt <topic> <text>        send an MT
    mthex <topic> <hex>      send an MT, bytes as hex
    signal <bars> [hidden]   send a constellationState, 0 to 5 bars
    topics <id>[,<id>...]    the provisioning from now on (empty: none)
    mo-fail <status>         the next MO ends with this final_mo_status
    mo-refuse                the next PUT messageOriginate is refused (406)
    mo-hold                  the next MO is taken but its final status is
                             withheld, as a modem waiting for a satellite,
                             until mo-release or a cancel
    mo-release               send the withheld final status
    segment <n>              ask for MO segments of n bytes (default 1446)
    state <state>            the operationalState from now on
    raw <text>               write text as it is, \r for CR: a line's
                             tail, junk, a line cut short
    before <text>            write text just before the next reply
    quit

    READY <port>
    RX <line>                every request from the host, as received
    TX <line>                every line sent to the host
    MO <topic> <id> <hex>    a whole MO with a good CRC
    MOBAD <topic> <id> <hex> a whole MO whose CRC did not match
    MT <topic> <id>          an MT sent

Run it in a process of its own beside Ground Control's Python binding: the
binding holds the GIL through every blocking call, so a fake in a thread of
the same process would freeze while the library waits on it.
"""

import argparse
import base64
import json
import os
import select
import sys
import threading
import time
import tty

MO_SEGMENT_MAX = 1446
MT_SEGMENT_MAX = 1083  # the library cuts MT data at 1446 base64 characters


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def with_crc(data):
    c = crc16_xmodem(data)
    return bytes(data) + bytes([c >> 8, c & 0xFF])


class FakeModem:
    """The modem's half of JSPR. Feed it bytes from the host with
    `feed()`; it writes replies through `write`, a function taking bytes.
    `events` is called with one string per event. Thread-safe: the control
    methods may be called from another thread than the one feeding."""

    def __init__(self, write, events=print, topics=(244,)):
        self.write = write
        self.events = events
        self.lock = threading.RLock()
        self.buf = b""
        self.topics = list(topics)
        self.state = "active"
        self.sim_interface = "internal"
        self.api_active = True
        self.bars, self.visible = 5, True
        self.next_id = 1
        self.segment = MO_SEGMENT_MAX
        self.mo = None          # the MO being received
        self.mo_fail = None     # final status for the next MO
        self.mo_refuse = False
        self.mo_hold = False    # the next MO's final status waits for mo-release
        self.held = None        # (topic, id, status) withheld
        self.before = b""       # written ahead of the next reply
        self.imei = "300434030000000"
        self.serial = "123456"
        self.iccid = "8988169712345678901"

    # --- the wire -------------------------------------------------------

    def send(self, code, target, obj):
        line = "%d %s %s" % (code, target, json.dumps(obj, separators=(",", ":")))
        self.events("TX " + line)
        self.write((line + "\r").encode())

    def feed(self, data):
        with self.lock:
            self.buf += data
            while True:
                i = self.buf.find(b"\r")
                if i < 0:
                    # The library ends lines with CR; tolerate LF from a person.
                    i = self.buf.find(b"\n")
                    if i < 0:
                        return
                line, self.buf = self.buf[:i], self.buf[i + 1:]
                line = line.strip(b"\r\n").decode(errors="replace")
                if line:
                    self.request(line)

    def request(self, line):
        self.events("RX " + line)
        if self.before:
            self.write(self.before)
            self.before = b""
        parts = line.split(" ", 2)
        if len(parts) < 2:
            return
        verb, target = parts[0], parts[1]
        if verb not in ("GET", "PUT"):
            return  # boot chatter from whatever is on the other end of the wire
        try:
            body = json.loads(parts[2]) if len(parts) > 2 else {}
        except ValueError:
            self.send(407, target, {})
            return
        handler = getattr(self, "on_%s_%s" % (verb.lower(), target), None)
        if handler is None:
            self.send(404, target, {})
            return
        handler(body)

    # --- requests -------------------------------------------------------

    def on_get_apiVersion(self, body):
        v = {"major": 1, "minor": 0, "patch": 0}
        reply = {"supported_versions": [v]}
        if self.api_active:
            reply["active_version"] = v
        self.send(200, "apiVersion", reply)

    def on_put_apiVersion(self, body):
        self.api_active = True
        self.send(200, "apiVersion", {"active_version": body.get("active_version", {})})

    def on_get_simConfig(self, body):
        self.send(200, "simConfig", {"interface": self.sim_interface})

    def on_put_simConfig(self, body):
        self.sim_interface = body.get("interface", "internal")
        self.send(200, "simConfig", {"interface": self.sim_interface})
        self.send(299, "simStatus", self.sim_status())

    def sim_status(self):
        return {"card_present": True, "sim_connected": True, "iccid": self.iccid}

    def on_get_simStatus(self, body):
        self.send(200, "simStatus", self.sim_status())

    def on_get_operationalState(self, body):
        self.send(200, "operationalState", {"state": self.state, "reason": 0})

    def on_put_operationalState(self, body):
        self.state = body.get("state", self.state)
        self.send(200, "operationalState", {"state": self.state})
        self.send(299, "operationalState", {"state": self.state, "reason": 0})

    def on_put_serviceConfig(self, body):
        self.send(200, "serviceConfig", {})

    def on_get_hwInfo(self, body):
        self.send(200, "hwInfo", {"hw_version": "1.0", "serial_number": self.serial,
                                  "imei": self.imei, "board_temp": 27})

    def on_get_firmware(self, body):
        self.send(200, "firmware", {"slot": body.get("slot", "primary"), "validity": 1,
                                    "version": {"major": 1, "minor": 2, "patch": 3,
                                                "build_info": "fake"},
                                    "hash": "0" * 64})

    def constellation(self):
        return {"constellation_visible": self.visible, "signal_bars": self.bars,
                "signal_level": -100 - (5 - self.bars) * 5}

    def on_get_constellationState(self, body):
        self.send(200, "constellationState", self.constellation())

    def on_get_messageProvisioning(self, body):
        self.send(200, "messageProvisioning", {"provisioning": [
            {"topic_id": t, "topic_name": "RAW" if t == 244 else "TOPIC%d" % t,
             "priority": "Low"} for t in self.topics]})

    def on_put_messageOriginate(self, body):
        topic, length = body.get("topic_id", 0), body.get("message_length", 0)
        if self.mo_refuse or topic not in self.topics or length < 3:
            self.mo_refuse = False
            self.send(406, "messageOriginate", {})
            return
        mid = self.take_id()
        self.mo = {"topic": topic, "id": mid, "len": length, "data": bytearray(length),
                   "next": 0}
        self.send(200, "messageOriginate", {"topic_id": topic,
                                            "request_reference": body.get("request_reference", 1),
                                            "message_id": mid,
                                            "message_response": "message_accepted"})
        self.ask_segment()

    def ask_segment(self):
        mo = self.mo
        n = min(self.segment, mo["len"] - mo["next"])
        self.send(299, "messageOriginateSegment", {"topic_id": mo["topic"], "message_id": mo["id"],
                                                   "segment_length": n,
                                                   "segment_start": mo["next"]})

    def on_put_messageOriginateSegment(self, body):
        mo = self.mo
        if mo is None or body.get("message_id") != mo["id"]:
            self.send(405, "messageOriginateSegment", {"message_id": body.get("message_id", 0)})
            return
        start, n = body.get("segment_start", 0), body.get("segment_length", 0)
        data = base64.b64decode(body.get("data", ""))
        mo["data"][start:start + len(data)] = data
        mo["next"] = start + n
        self.send(200, "messageOriginateSegment", {"topic_id": mo["topic"], "message_id": mo["id"]})
        if mo["next"] < mo["len"]:
            self.ask_segment()
            return
        whole, payload = bytes(mo["data"]), bytes(mo["data"][:-2])
        good = with_crc(payload) == whole
        self.events("%s %d %d %s" % ("MO" if good else "MOBAD", mo["topic"], mo["id"],
                                     payload.hex()))
        status = self.mo_fail or ("mo_ack_received" if good else "crc_error_in_transfer")
        self.mo_fail = None
        self.mo = None
        if self.mo_hold:
            self.mo_hold = False
            self.held = (mo["topic"], mo["id"], status)
            return
        self.mo_status(mo["topic"], mo["id"], status)

    def mo_status(self, topic, mid, status):
        self.send(299, "messageOriginateStatus", {"topic_id": topic, "message_id": mid,
                                                  "final_mo_status": status})

    def on_put_messageOriginateStatus(self, body):
        # Cancel: the library does not read the reply, only the status after.
        self.send(200, "messageOriginateStatus", {})
        if body.get("action") != "cancel":
            return
        if self.mo is not None:
            mo, self.mo = self.mo, None
            self.mo_status(mo["topic"], mo["id"], "message_cancelled_pre_transit")
        elif self.held is not None and self.held[1] == body.get("message_id"):
            topic, mid, _ = self.held
            self.held = None
            self.mo_status(topic, mid, "message_cancelled_in_transit")

    def release(self):
        with self.lock:
            if self.held is not None:
                topic, mid, status = self.held
                self.held = None
                self.mo_status(topic, mid, status)

    # --- what the network does --------------------------------------------

    def take_id(self):
        mid = self.next_id
        self.next_id = self.next_id % 255 + 1
        return mid

    def send_mt(self, topic, payload):
        with self.lock:
            whole = with_crc(payload)
            mid = self.take_id()
            self.send(299, "messageTerminate", {"topic_id": topic, "message_id": mid,
                                                "message_length_max": len(whole)})
            for start in range(0, len(whole), MT_SEGMENT_MAX):
                seg = whole[start:start + MT_SEGMENT_MAX]
                self.send(299, "messageTerminateSegment", {
                    "topic_id": topic, "message_id": mid, "segment_length": len(seg),
                    "segment_start": start, "data": base64.b64encode(seg).decode()})
            self.send(299, "messageTerminateStatus", {"topic_id": topic, "message_id": mid,
                                                      "final_mt_status": "complete"})
            self.events("MT %d %d" % (topic, mid))

    def set_signal(self, bars, visible=True):
        with self.lock:
            self.bars, self.visible = bars, visible
            self.send(299, "constellationState", self.constellation())

    def control(self, line):
        """One stdin command. Returns False on quit."""
        w = line.split(" ", 2)
        cmd = w[0] if w else ""
        if cmd == "mt":
            self.send_mt(int(w[1]), w[2].encode() if len(w) > 2 else b"")
        elif cmd == "mthex":
            self.send_mt(int(w[1]), bytes.fromhex(w[2]))
        elif cmd == "signal":
            self.set_signal(int(w[1]), not (len(w) > 2 and w[2] == "hidden"))
        elif cmd == "topics":
            with self.lock:
                self.topics = [int(t) for t in w[1].split(",")] if len(w) > 1 and w[1] else []
        elif cmd == "mo-fail":
            self.mo_fail = w[1]
        elif cmd == "mo-refuse":
            self.mo_refuse = True
        elif cmd == "mo-hold":
            self.mo_hold = True
        elif cmd == "mo-release":
            self.release()
        elif cmd == "segment":
            self.segment = max(1, min(MO_SEGMENT_MAX, int(w[1])))
        elif cmd == "state":
            self.state = w[1]
        elif cmd in ("raw", "before"):
            text = line.split(" ", 1)[1] if " " in line else ""
            data = text.encode().decode("unicode_escape").encode("latin-1")
            with self.lock:
                if cmd == "raw":
                    self.write(data)
                else:
                    self.before = data
            self.events("%s %s" % (cmd.upper(), text))
        elif cmd == "quit":
            return False
        elif cmd:
            self.events("ERR unknown command " + cmd)
        return True


# --- serving a port -----------------------------------------------------------


def open_pty():
    master, slave = os.openpty()
    tty.setraw(slave)
    path = os.ttyname(slave)
    # Keep the slave open ourselves so the master never sees EIO while the
    # host has it closed (the library opens, closes and opens it again).
    return master, slave, path


def serve(fd, modem, stop):
    while not stop.is_set():
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            continue
        try:
            data = os.read(fd, 4096)
        except OSError:
            time.sleep(0.05)  # EIO: no reader on the other side yet
            continue
        if data:
            modem.feed(data)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    ap.add_argument("--port", help="a real serial port instead of a pty")
    ap.add_argument("--baud", type=int, default=230400)
    ap.add_argument("--topics", default="244", help="provisioned topics, comma separated")
    ap.add_argument("--quiet", action="store_true", help="no RX/TX lines on stdout")
    ap.add_argument("--stay", action="store_true",
                    help="keep serving when stdin closes, until interrupted")
    a = ap.parse_args(argv)

    out_lock = threading.Lock()

    def events(s):
        if a.quiet and (s.startswith("RX ") or s.startswith("TX ")):
            return
        with out_lock:
            sys.stdout.write(s + "\n")
            sys.stdout.flush()

    if a.port:
        import serial
        port = serial.Serial(a.port, a.baud, timeout=0)
        fd, path, keep = port.fileno(), a.port, port
    else:
        fd, keep, path = open_pty()

    def write(b):
        os.write(fd, b)

    topics = [int(t) for t in a.topics.split(",") if t]
    modem = FakeModem(write, events, topics)
    stop = threading.Event()
    t = threading.Thread(target=serve, args=(fd, modem, stop), daemon=True)
    t.start()
    events("READY " + path)
    try:
        for line in sys.stdin:
            if not modem.control(line.strip()):
                break
        else:
            while a.stay:
                time.sleep(3600)
    except KeyboardInterrupt:
        pass
    stop.set()
    del keep
    return 0


if __name__ == "__main__":
    sys.exit(main())
