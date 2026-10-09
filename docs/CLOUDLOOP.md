# Cloudloop and a RockBLOCK 9704, from the box to a message

How to get a RockBLOCK 9704 from its box to a message seen on the
internet, from a computer over its USB-C port: the account, the token,
topics, provisioning, and the order to test things in. `rb9704` is the
command line in `python/` (`python/README.md` has every verb).

Written 2026-10-09 from Ground Control's and Cloudloop's documentation
and the library's source. `rb9704 info` and the free JSPR verbs have run
against a real 9704 since; the account, provisioning and airtime steps
have not been tried yet.

## What there is

- **The modem.** The 9704 is an Iridium IMT modem, the successor to SBD,
  with messages of up to 100 kB. It speaks JSPR, JSON over serial at a
  fixed 230400 8N1; there is no AT mode (`JSPR.md`).
- **Its USB-C port.** An FTDI FT234XD (USB id `0403:6015`) bridges it to
  the modem's UART, and the port also powers the board. It needs a port
  that supplies 500 mA at 4.0 to 5.3 V; an unpowered hub is not supported.
  The supercapacitor takes about 40 s to charge from USB. LEDs: red PWR is
  input voltage present, green RDY is the modem powered and booted, blue
  XMT is transmitting.
- **The library.** Ground Control's C library (MIT) is what this
  repository builds into Zephyr, pinned at v1.0.1 in `west.yml`. The
  `rockblock9704` Python package is a binding of the same C code, pinned
  at 1.0.2 in `python/pyproject.toml`. What works from the host is
  therefore the library the firmware runs.
- **Cloudloop Data.** Ground Control's service between the Iridium network
  and us. A message the modem sends (MO) lands in Cloudloop, which
  delivers it to a destination; a message to the modem (MT) is queued in
  Cloudloop through its web interface or its API.

## The accounts

There is one account to open, Cloudloop, and one token to ask for.

### 1. The Cloudloop account and the airtime

The 9704 arrives with an activation card.

1. Go to <https://join.cloudloop.com> and create an account. The account
   holds the billing card and every modem on it, so make it under the
   business and an address more than one person reads.
2. Follow the prompts to add the RockBLOCK 9704 from its activation card,
   choose a data package and activate the airtime.
3. A **Thing** is made for the modem when it is enrolled. A Thing is
   Cloudloop's name for one device, whatever its network, and it has a 32
   character case-sensitive id. `rb9704 cloud things` lists
   them once the token below is in place.

The data packages and their prices change, so they are not copied here;
the Ground Control store and the Cloudloop billing page are where to read
them. Check whether the package charges by the month while the line is
active, and if it does, ask Ground Control how to suspend it for a modem
that will sit unused between bench sessions.

### 2. A Thing Group

Destinations attach to Thing Groups and never to a Thing alone. In the
Cloudloop web interface, make a group (say `bench`) and add the
modem's Thing to it. A fleet can be one group per project, so a destination
serves a project.

### 3. The API token

The API is how the tool reads messages and sends MT, and how the shore
side will. A token is asked for, not made:

1. Email <help@groundcontrol.com> for a Cloudloop **platform API token**
   for the account. Ask for one that can send MT: `DoSendImtMessage` needs
   the Admin permission, reading needs only Read Only.
2. When it arrives, replace it with a fresh one that nobody else has seen,
   with `User/DoGenerateToken`, and keep the new one: a token cannot be
   recovered once generated.
3. Store it outside any repository, readable by you alone:

   ```sh
   mkdir -p ~/.config/rb9704
   printf '%s\n' '<token>' > ~/.config/rb9704/cloudloop.token
   chmod 600 ~/.config/rb9704/cloudloop.token
   rb9704 cloud ping                          # {"ping": "pong"}
   ```

   `CLOUDLOOP_TOKEN` in the environment overrides the file.
   `CLOUDLOOP_THING` saves typing `--thing` on every cloud verb.

The token goes in the query string of every call, so it is in any proxy
log on the path; the API is HTTPS only, and nothing here logs it.

### 4. A destination, later

On the bench a destination is not needed: every MO is kept in Cloudloop
and `cloud mo` and `cloud msg` read it back. For the shore side,
Cloudloop pushes each MO to an **HTTP webhook** destination:

- URL: your server's route for messages from the modem.
- Format: **JSON (Lingo)**, the one Cloudloop recommends. The message
  bytes are in it base64-encoded, beside the identity and the receive
  time. The first MO will show it with `rb9704 cloud msg <id>`, which is
  the sample for whoever writes the receiving route.
- Certificate validation on.
- The receiver must answer HTTP 200 within 5 seconds. Cloudloop retries
  with exponential backoff, from 2 s, for about 12 hours, from
  35.178.100.117 and 52.56.155.169. Those two addresses are the only
  authentication the documentation describes, so the route should check
  them and the IMEI against the modems it expects.

Add the destination to the Thing Group. The web interface shows its
health (grey unknown, green OK, orange some failures, red all failing).

## Topics

All IMT traffic goes through a **topic**, a numbered channel. 244 is the
raw topic, the default for everything here. Cloudloop also provisions the
colour topics, 313 purple to 317 yellow, which Cloudloop can route
separately. A modem may only send on topics it has been provisioned
with. The library checks this before it transmits and refuses anything
else, including any id below 64, without spending airtime.

In the Cloudloop API the same topics have names: `IMT_TOPIC_RAW` for 244,
`IMT_TOPIC_PURPLE` and so on.

## Provisioning

A new modem knows no topics. After the airtime is activated it needs a
clear view of the sky for 10 to 30 minutes, or more, while it collects
its provisioning from the network. Indoors this never finishes. Do it
once, outside, with the modem on a laptop:

```sh
rb9704 --log provision.jsonl provision
```

This asks the modem for its signal and its provisioning every 30 s and
stops when topics appear, printing them. It talks JSPR directly, because
the library only asks for provisioning when it is about to send, and
asking this way costs nothing.

## The order to test in

Each step needs the one before it. Only the steps marked airtime cost
money; the tool asks before each one, and `--yes` skips the question.

1. **Install.** `python/install.sh --udev`. The `--udev` rule
   keeps ModemManager off the port and names it `/dev/rockblock9704`; it
   needs sudo once.
2. **Find it.** Plug the USB-C in, wait for green RDY, then
   `rb9704 ports`. The 9704 is the FT234XD, listed first.
3. **Identity, indoors.** `rb9704 info` gives hardware and
   firmware versions, serial, IMEI, ICCID, whether the SIM is present and
   talking, and the board temperature. Record the IMEI and ICCID against
   the Thing in Cloudloop.
4. **Signal, outdoors.** `rb9704 signal --watch 600` reads
   the bars (0 to 5) every 5 s for ten minutes and ends with how often
   each was seen. Bars move a lot as satellites pass; zero for a while is
   normal.
5. **Provision**, as above.
6. **One MO** (airtime). `rb9704 send --text "hello"`.
   It should appear in the Cloudloop web interface and in
   `rb9704 cloud mo`. Then `cloud msg <id>` shows the Lingo
   form, which is the sample for the receiving route.
7. **One MT** (airtime). With `rb9704 listen --out mt` running beside
   the modem, `rb9704 cloud mt --text "ping"` from any machine. The MT
   arrives in `mt/` and `cloud status <id>` reports
   its delivery. The modem has no store for MT: one that arrives while
   nothing is listening is lost, so listen first.
8. **A session** (airtime). Queue two or three MT with `cloud mt`, then
   `rb9704 --log session.jsonl session --text status --quiet 90`. It
   sends one MO and collects MT until 90 s pass with nothing new. The log
   says how long after an MO the queued MT arrive and how far apart, and
   so what quiet window a firmware needs after it sends.

`--log` on any verb appends one JSON line per event, with UTC times, for
the bench log.

## When begin fails

`begin()` failing means the library could not finish the setup (API
version, SIM, operational state). In order of likelihood:

- **The port is held.** ModemManager probes a new USB serial port for
  a few seconds after it appears, and any open terminal on the port
  blocks it. The `--udev` rule stops the first.
- **Not in `dialout`.** `install.sh` checks; `sudo usermod -aG dialout
  $USER` and log in again.
- **Not booted.** No green RDY: the port gives too little current (a hub,
  a front-panel port), or the supercapacitor is still charging.
- **Something odd.** `rb9704 jspr --setup 'GET hwInfo {}'
  'GET simStatus {}'` shows the modem's own answers; a 4xx code names
  the problem (`409` unauthorized, `410` SIM not configured, `418` not
  provisioned).
