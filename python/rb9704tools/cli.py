# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
"""
rb9704 - drive a RockBLOCK 9704 from the host over its USB-C port.

    rb9704 ports
    rb9704 [-p PORT] info
    rb9704 [-p PORT] signal [--watch S] [--every S]
    rb9704 [-p PORT] provision [--timeout S]
    rb9704 [-p PORT] jspr [LINE ...]
    rb9704 [-p PORT] send   (--text T | --hex H | --file F) [--topic N] [--yes]
    rb9704 [-p PORT] listen [--out DIR] [--for S]
    rb9704 [-p PORT] session (--text T | --hex H | --file F) [--quiet S] [--yes]
    rb9704 cloud ping | things | mo | msg ID | mt ... | status ID

The modem side uses Ground Control's rockblock9704 package, a binding of
the same C library this repository builds into Zephyr, so what works here
is the library the firmware runs. `provision` and `jspr` speak
JSPR directly over pyserial instead, because the library only asks for
the provisioning when a message is sent, and asking that way is free.

`send` and `session` cost airtime and ask before sending; --yes skips the
question. Everything else on the modem side is local serial traffic and
costs nothing. `cloud mt` costs airtime too.

The port is -p, or $RB9704_PORT, or the one FTDI FT234XD attached (the
9704's USB-C bridge, 0403:6015). --log FILE appends every event as a JSON
line, for the bench log.

The cloud verbs talk to the Cloudloop Data API (api.cloudloop.com). The
token is $CLOUDLOOP_TOKEN or the first line of ~/.config/rb9704/cloudloop.token;
it never goes in a repository. docs/CLOUDLOOP.md says how to get one.
"""
import argparse
import base64
import datetime as dt
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

FTDI_VID, FT234X_PID = 0x0403, 0x6015
BAUD = 230400
RAW_TOPIC = 244
CLOUDLOOP = "https://api.cloudloop.com"
TOKEN_FILE = os.path.expanduser("~/.config/rb9704/cloudloop.token")

LOG = None


def now():
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def ev(kind, **kw):
    """Print an event and append it to --log as one JSON line."""
    rec = {"t": now(), "ev": kind, **kw}
    shown = " ".join(f"{k}={v}" for k, v in kw.items())
    print(f"{rec['t']}  {kind:<10} {shown}", flush=True)
    if LOG:
        with open(LOG, "a") as f:
            f.write(json.dumps(rec) + "\n")


def die(msg, code=1):
    print(f"!! {msg}", file=sys.stderr)
    sys.exit(code)


# ---------------------------------------------------------------- ports

def find_ports():
    """Every serial port, with the ones that look like a 9704 first."""
    try:
        from serial.tools import list_ports
    except ImportError:
        die("pyserial is missing; run python/install.sh")
    out = []
    for p in list_ports.comports():
        is_rb = p.vid == FTDI_VID and p.pid == FT234X_PID
        out.append((is_rb, p))
    out.sort(key=lambda x: (not x[0], x[1].device))
    return out


def pick_port(arg):
    if arg:
        return arg
    if os.environ.get("RB9704_PORT"):
        return os.environ["RB9704_PORT"]
    rbs = [p for is_rb, p in find_ports() if is_rb]
    if len(rbs) == 1:
        return rbs[0].device
    if not rbs:
        die("no FTDI FT234XD attached; plug the 9704's USB-C in, or give -p PORT "
            "(rb9704 ports lists them)")
    die("more than one FT234XD attached (" + ", ".join(p.device for p in rbs) +
        "); give -p PORT")


def cmd_ports(a):
    # the motherboard's ttyS ports have no USB ids and are never the 9704
    ports = [x for x in find_ports() if a.all or x[1].vid is not None]
    if not ports:
        print("no serial ports")
        return
    for is_rb, p in ports:
        ids = f"{p.vid:04x}:{p.pid:04x}" if p.vid is not None else "-"
        mark = "9704?" if is_rb else ""
        print(f"{p.device:<16} {ids:<10} {mark:<6} {p.serial_number or '':<12} {p.description}")
    if os.path.isdir("/dev/serial/by-id"):
        for n in sorted(os.listdir("/dev/serial/by-id")):
            if "FT234X" in n:
                print(f"  by-id: /dev/serial/by-id/{n}")


# ---------------------------------------------------------------- library

def open_rb(port):
    try:
        from rockblock9704 import RockBlock9704
    except ImportError:
        die("rockblock9704 is missing; run python/install.sh")
    rb = RockBlock9704()
    if not rb.begin(port):
        die(f"begin({port}) failed: wrong port, modem not booted (green RDY "
            "LED), or another program holds the port (ModemManager, a "
            "terminal). docs/CLOUDLOOP.md, \"When begin fails\"")
    return rb


def cmd_info(a):
    port = pick_port(a.port)
    rb = open_rb(port)
    try:
        info = {
            "port": port,
            "hardware": rb.get_hardware_version(),
            "firmware": rb.get_firmware_version(),
            "serial": rb.get_serial_number(),
            "imei": rb.get_imei(),
            "iccid": rb.get_iccid(),
            "card_present": rb.get_card_present(),
            "sim_connected": rb.get_sim_connected(),
            "board_temp_c": rb.get_board_temp(),
            "signal": rb.get_signal(),
        }
    finally:
        rb.end()
    if a.json:
        print(json.dumps(info, indent=2))
    else:
        for k, v in info.items():
            print(f"{k:<14} {v}")
    if LOG:
        ev("info", **info)


def cmd_signal(a):
    port = pick_port(a.port)
    rb = open_rb(port)
    try:
        if not a.watch:
            ev("signal", bars=rb.get_signal())
            return
        end = time.monotonic() + a.watch
        seen = {}
        while time.monotonic() < end:
            bars = rb.get_signal()
            seen[bars] = seen.get(bars, 0) + 1
            ev("signal", bars=bars)
            time.sleep(a.every)
        total = sum(seen.values())
        ev("summary", samples=total,
           **{f"bars{k}": f"{100 * v / total:.0f}%" for k, v in sorted(seen.items())})
    except KeyboardInterrupt:
        pass
    finally:
        rb.end()


def payload(a):
    if a.text is not None:
        return a.text.encode()
    if a.hex is not None:
        return bytes.fromhex(a.hex)
    with open(a.file, "rb") as f:
        return f.read()


def confirm(data, topic, yes):
    print(f"AIRTIME: {len(data)} bytes to topic {topic}. This costs money.", flush=True)
    if yes:
        return
    if not sys.stdin.isatty():
        die("not a terminal; pass --yes to send")
    if input("send? [y/N] ").strip().lower() not in ("y", "yes"):
        die("not sent", 0)


def cmd_send(a):
    data = payload(a)
    port = pick_port(a.port)
    confirm(data, a.topic, a.yes)
    rb = open_rb(port)
    try:
        ev("signal", bars=rb.get_signal())
        t0 = time.monotonic()
        ok = rb.send_message(data, topic=None if a.topic == RAW_TOPIC else a.topic,
                             timeout=a.timeout)
        secs = time.monotonic() - t0
        ev("mo", ok=ok, bytes=len(data), topic=a.topic, secs=f"{secs:.1f}")
        if not ok and secs < 2:
            # the library checks provisioning before it queues anything, and
            # gives up at once if the topic is not there: nothing was sent
            print(f"   refused before transmitting: the modem holds no "
                  f"provisioning for topic {a.topic}. Run provision under sky.")
    finally:
        rb.end()
    sys.exit(0 if ok else 2)


class Async:
    """The library's callback API, polled from one loop. The callbacks only
    record; all the work is done in the loop, between polls."""

    def __init__(self, port):
        self.rb = None
        self.mo_done = None
        self.mo_id = None
        self.mt_ready = 0
        self.bars = None
        self.port = port

    def start(self):
        from rockblock9704 import RockBlock9704
        rb = RockBlock9704()
        rb.set_message_provisioning_callback(self._prov)
        rb.set_mo_message_started_callback(self._mo_started)
        rb.set_mo_message_complete_callback(self._mo)
        rb.set_mt_message_complete_callback(self._mt)
        rb.set_constellation_state_callback(self._sig)
        if not rb.begin(self.port):
            die(f"begin({self.port}) failed")
        time.sleep(0.1)  # the library asks for 100 ms before the first queue
        self.rb = rb
        return rb

    def _prov(self, p):
        ev("prov", set=p["provisioningSet"], topics=",".join(
            f"{t['topicId']}:{t['topicName']}" for t in p["provisioning"]))

    def _mo_started(self, mid):
        self.mo_id = mid
        ev("mo_start", id=mid)

    def _mo(self, mid, status):
        self.mo_done = status == 1
        ev("mo", id=mid, ok=self.mo_done)

    def _mt(self, mid, status):
        ev("mt_done", id=mid, ok=status == 1)
        if status == 1:
            self.mt_ready += 1

    def _sig(self, s):
        if s["signalBars"] != self.bars:
            self.bars = s["signalBars"]
            ev("signal", bars=self.bars, level=s["signalLevel"],
               visible=s["constellationVisible"])

    def drain(self, out):
        """Collect every completed MT. Returns how many."""
        n = 0
        while self.mt_ready:
            msg = self.rb.receive_message_async()
            if msg is None:
                break
            self.rb.acknowledge_receive_head_async()
            self.mt_ready -= 1
            n += 1
            name = None
            if out:
                os.makedirs(out, exist_ok=True)
                name = os.path.join(out, dt.datetime.now(dt.timezone.utc)
                                    .strftime("MT-%Y%m%dT%H%M%S.%fZ.bin"))
                with open(name, "wb") as f:
                    f.write(msg)
            ev("mt", bytes=len(msg), head=msg[:24].hex(), file=name or "-")
        return n


def cmd_listen(a):
    s = Async(pick_port(a.port))
    rb = s.start()
    end = time.monotonic() + a.for_ if a.for_ else None
    got = 0
    ev("listen", port=s.port, out=a.out or "-")
    try:
        while end is None or time.monotonic() < end:
            rb.poll()
            got += s.drain(a.out)
            time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        rb.end()
    ev("summary", mt=got)


def cmd_session(a):
    """One MO, then listen until a.quiet seconds pass with no new MT: a
    buoy's or a hub's session, run from the host so the quiet window can
    be chosen from what is seen."""
    data = payload(a)
    port = pick_port(a.port)
    confirm(data, a.topic, a.yes)
    s = Async(port)
    rb = s.start()
    got = 0
    try:
        if not rb.send_message_async(data, a.topic):
            die("the modem would not queue the message (not provisioned for "
                f"topic {a.topic}? run provision)")
        ev("mo_queue", bytes=len(data), topic=a.topic)
        t0 = time.monotonic()
        while s.mo_done is None:
            rb.poll()
            got += s.drain(a.out)
            if time.monotonic() - t0 > a.timeout:
                if s.mo_id is not None:
                    rb.cancel_message(a.topic, s.mo_id)
                ev("mo", ok=False, why="timeout", secs=a.timeout)
                break
            time.sleep(0.01)
        ev("mo_secs", secs=f"{time.monotonic() - t0:.1f}")
        last = time.monotonic()
        while time.monotonic() - last < a.quiet:
            rb.poll()
            n = s.drain(a.out)
            if n:
                got += n
                last = time.monotonic()
            time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        rb.end()
    ev("summary", mo_ok=s.mo_done, mt=got, quiet=a.quiet)
    sys.exit(0 if s.mo_done else 2)


# ---------------------------------------------------------------- raw JSPR

class Jspr:
    """JSPR by hand: one line out, `<code> <target> <json>` lines back, all
    ending in CR. 299 is unsolicited. The same setup as the library's
    rbBegin: an active API version, the internal SIM, state active."""

    def __init__(self, port):
        import serial
        try:
            self.s = serial.Serial(port, BAUD, timeout=0.1)
        except serial.SerialException as e:
            die(f"cannot open {port}: {e}")
        self.buf = b""
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def lines(self, secs):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            self.buf += self.s.read(4096)
            while b"\r" in self.buf:
                line, self.buf = self.buf.split(b"\r", 1)
                # bootInfo can arrive behind a DC1; the library strips it too
                line = line.lstrip(b"\x11\n ").decode(errors="replace")
                if len(line) >= 4:
                    yield parse(line)

    def cmd(self, line, target=None, secs=3.0, show=False):
        """Send a line, return the first response for its target. Anything
        else that arrives meanwhile is shown as it goes past."""
        words = line.split()
        if target is None and len(words) < 2:
            die(f"not a JSPR line: {line!r} (verb, target, json)")
        target = target or words[1]
        self.s.write((line + "\r").encode())
        for code, tgt, js, raw in self.lines(secs):
            if show or tgt != target or code == 299:
                print(f"  < {raw}")
            if tgt == target and code != 299:
                return code, js
        return None, None

    def setup(self):
        code, js = self.cmd("GET apiVersion {}")
        if code != 200:
            die(f"GET apiVersion answered {code}: is this a 9704 at {BAUD}?")
        if not js.get("active_version"):
            v = js["supported_versions"][0]
            self.cmd("PUT apiVersion " + json.dumps({"active_version": v}))
        code, js = self.cmd("GET simConfig {}")
        if js.get("interface") != "internal":
            self.cmd('PUT simConfig {"interface": "internal"}')
        code, js = self.cmd("GET operationalState {}")
        st = js.get("state")
        if st != "active":
            if st != "inactive":
                self.cmd('PUT operationalState {"state": "inactive"}')
            self.cmd('PUT operationalState {"state": "active"}')


def parse(line):
    code, _, rest = line.partition(" ")
    tgt, _, js = rest.partition(" ")
    try:
        return int(code), tgt, json.loads(js) if js else {}, line
    except ValueError:
        return 0, "", {}, line


def cmd_provision(a):
    port = pick_port(a.port)
    j = Jspr(port)
    t0 = time.monotonic()
    try:
        j.setup()
        while True:
            code, sig = j.cmd("GET constellationState {}")
            code, prov = j.cmd("GET messageProvisioning {}")
            topics = (prov or {}).get("provisioning") or []
            ev("prov", code=code, set=bool(topics),
               bars=(sig or {}).get("signal_bars"),
               visible=(sig or {}).get("constellation_visible"),
               topics=",".join(f"{t.get('topic_id')}:{t.get('topic_name')}"
                               for t in topics) or "-")
            if topics:
                print(json.dumps(prov, indent=2))
                return
            if time.monotonic() - t0 > a.timeout:
                die(f"not provisioned after {a.timeout} s; a clear sky view "
                    "for 10 to 30 minutes is what it needs", 2)
            for _ in j.lines(a.every):
                pass
    except KeyboardInterrupt:
        pass
    finally:
        j.close()


def cmd_jspr(a):
    """Lines from the arguments, or typed, one at a time. Responses and
    anything unsolicited are printed as they arrive."""
    j = Jspr(pick_port(a.port))
    try:
        if a.setup:
            j.setup()
        if a.line:
            for line in a.line:
                print(f"  > {line}")
                j.cmd(line, show=True, secs=a.wait)
            return
        print("JSPR at %d. A line such as: GET hwInfo {}   (ctrl-d to quit)" % BAUD)
        while True:
            try:
                line = input("> ").strip()
            except EOFError:
                return
            if line:
                j.cmd(line, show=True, secs=a.wait)
            else:
                for _, _, _, raw in j.lines(1.0):
                    print(f"  < {raw}")
    except KeyboardInterrupt:
        pass
    finally:
        j.close()


# ---------------------------------------------------------------- Cloudloop

def token():
    t = os.environ.get("CLOUDLOOP_TOKEN")
    if not t and os.path.exists(TOKEN_FILE):
        with open(TOKEN_FILE) as f:
            t = f.readline().strip()
    if not t:
        die(f"no Cloudloop token: set CLOUDLOOP_TOKEN or write {TOKEN_FILE} "
            "(docs/CLOUDLOOP.md, \"The API token\")")
    return t


def api(path, **params):
    """Every Cloudloop call is a POST with its arguments, the token among
    them, in the query string."""
    q = urllib.parse.urlencode({"token": token(), **params})
    req = urllib.request.Request(f"{CLOUDLOOP}{path}?{q}", data=b"", method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = json.load(r)
    except urllib.error.HTTPError as e:
        die(f"{path}: HTTP {e.code} {e.reason}" +
            (": the token was not accepted" if e.code in (401, 403) else ""))
    except (urllib.error.URLError, TimeoutError) as e:
        die(f"{path}: {getattr(e, 'reason', e)}")
    except ValueError:
        die(f"{path}: the reply was not JSON")
    # Failures come back as HTTP 200 with the exception named in the body:
    # {"error": "DispatcherAuthenticationException"} for a bad token, or the
    # exception's name as a key with its details under it.
    if isinstance(body, dict):
        if body.get("error"):
            die(f"{path}: {body['error']}" +
                (" (the token was not accepted)" if "Authentication" in str(body["error"])
                 else ""))
        for k in body:
            if k.startswith("Dispatcher") and k.endswith("Exception"):
                die(f"{path}: {k} {json.dumps(body[k])}")
    return body


def thing(a):
    t = a.thing or os.environ.get("CLOUDLOOP_THING")
    if not t:
        die("which thing? --thing ID or $CLOUDLOOP_THING (cloud things lists them)")
    return t


def stamp(d):
    return d.strftime("%Y-%m-%d %H:%M:%S")


def cmd_cloud(a):
    if a.verb == "ping":
        print(json.dumps(api("/Platform/Ping")))
    elif a.verb == "things":
        for t in api("/Data/GetThings").get("things", []):
            print(f"{t.get('id')}  certus={t.get('subscriberCertus') or '-'}  "
                  f"sbd={t.get('subscriberSbd') or '-'}  {t.get('name', '')}")
    elif a.verb == "mo":
        to = dt.datetime.now(dt.timezone.utc)
        frm = to - dt.timedelta(hours=a.hours)
        recs = api("/Data/GetMessageRecordsForThing", thing=thing(a),
                   **{"from": stamp(frm), "to": stamp(to)}).get("messageRecords", [])
        for r in recs:
            if a.json:
                print(json.dumps(r))
            else:
                print(f"{r.get('id')}  {r.get('receivedAt', r.get('createdAt', ''))}  "
                      f"{r.get('type')}  {r.get('direction', '')}  "
                      f"{(r.get('snippet') or '')[:40]}")
        print(f"{len(recs)} record(s) in the last {a.hours} h", file=sys.stderr)
    elif a.verb == "msg":
        print(json.dumps(api("/Data/GetMessageRecord", messageRecord=a.id), indent=2))
        print(json.dumps(api("/Data/GetLingoMo", messageRecord=a.id), indent=2))
    elif a.verb == "status":
        print(json.dumps(api("/Data/GetMtDeliveryStatus", message=a.id)))
    elif a.verb == "mt":
        data = payload(a)
        token()
        confirm(data, a.topic, a.yes)
        r = api("/Data/DoSendImtMessage", thing=thing(a), topic=a.topic,
                ringStyle=a.ring, message=base64.b64encode(data).decode())
        ev("cloud_mt", bytes=len(data), topic=a.topic, result=json.dumps(r))


# ---------------------------------------------------------------- main

def add_payload(p, required=True):
    g = p.add_mutually_exclusive_group(required=required)
    g.add_argument("--text", help="the message as UTF-8 text")
    g.add_argument("--hex", help="the message as hex bytes")
    g.add_argument("--file", help="the message is this file's bytes")
    p.add_argument("--yes", action="store_true", help="send without asking")


def main():
    global LOG
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip(),
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    # -p and --log go before or after the verb; SUPPRESS keeps a verb's
    # default from hiding a value given before it
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-p", "--port", default=argparse.SUPPRESS,
                        help="serial port (default: $RB9704_PORT or the FT234XD)")
    common.add_argument("--log", default=argparse.SUPPRESS,
                        help="append every event to this file as JSON lines")
    ap.add_argument("-p", "--port", help=argparse.SUPPRESS)
    ap.add_argument("--log", help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    real_add = sub.add_parser
    sub.add_parser = lambda *x, **k: real_add(*x, parents=[common], **k)

    p = sub.add_parser("ports", help="list USB serial ports, the 9704 first")
    p.add_argument("--all", action="store_true", help="the ttyS ports too")
    p.set_defaults(fn=cmd_ports)

    p = sub.add_parser("info", help="identity, SIM and signal (free)")
    p.add_argument("--json", action="store_true")
    p.set_defaults(fn=cmd_info)

    p = sub.add_parser("signal", help="signal bars 0-5 (free)")
    p.add_argument("--watch", type=float, default=0, help="keep reading for S seconds")
    p.add_argument("--every", type=float, default=5, help="seconds between reads")
    p.set_defaults(fn=cmd_signal)

    p = sub.add_parser("provision", help="wait until the modem holds its topics (free)")
    p.add_argument("--timeout", type=float, default=3600)
    p.add_argument("--every", type=float, default=30)
    p.set_defaults(fn=cmd_provision)

    p = sub.add_parser("jspr", help="raw JSPR lines, typed or given")
    p.add_argument("line", nargs="*")
    p.add_argument("--setup", action="store_true",
                   help="set the API version, SIM and state first, as begin does")
    p.add_argument("--wait", type=float, default=3, help="seconds to wait for each answer")
    p.set_defaults(fn=cmd_jspr)

    p = sub.add_parser("send", help="one MO message, blocking (COSTS AIRTIME)")
    add_payload(p)
    p.add_argument("--topic", type=int, default=RAW_TOPIC)
    p.add_argument("--timeout", type=int, default=120)
    p.set_defaults(fn=cmd_send)

    p = sub.add_parser("listen", help="collect MT messages as they arrive (free)")
    p.add_argument("--out", help="write each MT here as MT-<utc>.bin")
    p.add_argument("--for", dest="for_", type=float, default=0, help="stop after S seconds")
    p.set_defaults(fn=cmd_listen)

    p = sub.add_parser("session", help="one MO, then MT until a quiet window (COSTS AIRTIME)")
    add_payload(p)
    p.add_argument("--topic", type=int, default=RAW_TOPIC)
    p.add_argument("--timeout", type=int, default=180, help="give up on the MO after S seconds")
    p.add_argument("--quiet", type=float, default=60, help="end after S seconds with no new MT")
    p.add_argument("--out", help="write each MT here as MT-<utc>.bin")
    p.set_defaults(fn=cmd_session)

    p = sub.add_parser("cloud", help="the Cloudloop Data API")
    cs = p.add_subparsers(dest="verb", required=True)
    cs.add_parser("ping")
    cs.add_parser("things")
    q = cs.add_parser("mo", help="message records for a thing")
    q.add_argument("--thing")
    q.add_argument("--hours", type=float, default=24)
    q.add_argument("--json", action="store_true")
    q = cs.add_parser("msg", help="one message record and its Lingo form")
    q.add_argument("id")
    q = cs.add_parser("status", help="delivery status of an MT")
    q.add_argument("id")
    q = cs.add_parser("mt", help="queue an MT to the modem (COSTS AIRTIME)")
    q.add_argument("--thing")
    q.add_argument("--topic", default="IMT_TOPIC_RAW")
    q.add_argument("--ring", default="RING_STYLE_NORMAL")
    add_payload(q)
    p.set_defaults(fn=cmd_cloud)

    a = ap.parse_args()
    LOG = a.log
    a.fn(a)


if __name__ == "__main__":
    main()
