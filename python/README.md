# rockblock9704-tools: the RockBLOCK 9704 from a computer

Two commands and a test suite, in one Python package:

- `rb9704`, a command line for a RockBLOCK 9704 plugged into USB and for
  the Cloudloop account behind it, for proving the modem, the account and
  the delivery path with no firmware in the loop;
- `rb9704-fake`, a fake modem that speaks JSPR on a pty or a serial port,
  for testing anything that drives a 9704 without one;
- `tests/`, which run Ground Control's own Python binding, Zephyr on
  `native_sim`, and Zephyr on a board against the fake
  (`../docs/TESTING.md`).

`../docs/CLOUDLOOP.md` is the account, the provisioning and the order to
test things in; this is the tools.

## Install

```sh
python/install.sh            # python/.venv, this package, rockblock9704, pytest
python/install.sh --udev     # ... and keep ModemManager off the port (sudo)
```

Or by hand: `pip install -e 'python[modem,test]'`. Python 3.10 to 3.14,
which is where `rockblock9704` has wheels (Linux x86_64 and aarch64,
macOS, Windows). Without the `modem` extra, the fake modem, `jspr`,
`provision` and the cloud verbs work and the library verbs do not.

## Verbs

| Verb | Costs | What it does |
| --- | --- | --- |
| `ports` | | USB serial ports, the FT234XD (`0403:6015`) first |
| `info [--json]` | | hardware and firmware, serial, IMEI, ICCID, SIM, temperature, signal |
| `signal [--watch S] [--every S]` | | signal bars 0 to 5, once or for S seconds with a summary |
| `provision [--timeout S]` | | the setup `begin` does, then signal and provisioning every 30 s until topics appear |
| `jspr [--setup] [LINE ...]` | | raw JSPR: the lines given, or a prompt; every answer and unsolicited line printed |
| `send --text/--hex/--file [--topic N]` | airtime | one MO, blocking, through the library |
| `listen [--out DIR] [--for S]` | | MT as it arrives, each written to `DIR/MT-<utc>.bin` |
| `session --text/--hex/--file [--quiet S]` | airtime | one MO, then MT until S seconds pass with nothing new |
| `cloud ping` | | the token works |
| `cloud things` | | every Thing on the account, with its Certus subscriber id |
| `cloud mo [--thing ID] [--hours H]` | | message records for a Thing over the last H hours |
| `cloud msg ID` | | one message record and its Lingo form |
| `cloud mt --text/--hex/--file [--thing ID]` | airtime | queue an MT on `IMT_TOPIC_RAW` |
| `cloud status ID` | | an MT's delivery status |
| `pattern make [--size N] [--out F]` | | an `rbtest` test message, as hex or a file, to queue with `cloud mt` |
| `pattern check --hex/--b64/--file` | | check every byte of one, as Cloudloop shows it |

The verbs that cost airtime print the size and ask; `--yes` skips the
question and is the only way to send from a script.

Options that go with any verb, before or after it:

- `-p PORT`: the serial port. Otherwise `$RB9704_PORT`, otherwise the one
  9704 bridge attached (`0403:6015`); two attached is an error.
- `--log FILE`: append one JSON line per event (`{"t": ..., "ev": ...}`,
  UTC), for the bench log.

The cloud verbs read the token from `$CLOUDLOOP_TOKEN` or
`~/.config/rb9704/cloudloop.token`, and the Thing from `--thing` or
`$CLOUDLOOP_THING`.

## The fake modem

```sh
rb9704-fake                        # a pty; prints READY /dev/pts/N
rb9704-fake --port /dev/ttyUSB0    # a real port, e.g. an adapter wired to a board
rb9704-fake --topics 244,313       # the provisioning it reports
```

It answers everything Ground Control's library asks (`../docs/JSPR.md`),
takes MO messages segment by segment and checks their CRC, and on a
command on stdin sends an MT, a signal change, a refusal or a failed MO.
The commands and the events it prints are in `rb9704tools/fakemodem.py`'s
docstring. Run it in a process of its own beside the library's Python
binding, which holds the GIL through every blocking call.

## How it is built

- The modem verbs (`info`, `signal`, `send`, `listen`, `session`) use
  Ground Control's `rockblock9704`, a binding of the C library this
  repository builds into Zephyr. `listen` and `session` use its
  callback API, polled every 10 ms as the library asks, with the
  callbacks only recording and the loop doing the work.
- `provision` and `jspr` speak JSPR over pyserial, because the library
  asks for the provisioning only inside a send. `Jspr.setup` repeats
  `rbBegin`'s sequence: an active API version, the internal SIM, the
  state set active. JSPR lines are `<code> <target> <json>` ending in CR;
  299 is unsolicited.
- The cloud verbs are plain `urllib` POSTs to `api.cloudloop.com`, every
  argument in the query string as the API's Swagger
  (<https://api.cloudloop.com/swagger/index.html>) describes it.

Only one program can hold the port: `jspr` and the library verbs cannot
run at once.
