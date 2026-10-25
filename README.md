# rockblock9704-zephyr

The RockBLOCK 9704, Ground Control's Iridium Certus modem, on Zephyr: a
module that builds Ground Control's own C library for any board with a
UART, gives it the one thread it needs, and offers a small API, a shell
command and a devicetree binding. With it come host tools in Python and a
fake modem, so the same tests run against Ground Control's Python binding,
against Zephyr on `native_sim`, and against Zephyr on a board.

The 9704 sends and receives IMT messages of up to 100 kB through the
Iridium network, and speaks JSPR, JSON lines over a 230400 8N1 UART. There
is no AT mode; it is not a 9602 or 9603.

rockblock9704-zephyr is written and maintained by Scott Penrose
([Digital Dimensions](https://www.dd.com.au/)). It was built for two
projects that send data home by satellite:
[WII6](https://wii6.au/), a wave-measuring buoy, and
[OGLAS](https://oglas.au/), a fleet of LoRa sensors, repeaters and hubs.
Both run it, and neither is needed to use it.

The source lives at
[gitea.sh3d.com.au/Sh3d/rockblock9704-zephyr](https://gitea.sh3d.com.au/Sh3d/rockblock9704-zephyr)
and is mirrored to
[github.com/SH3D/rockblock9704-zephyr](https://github.com/SH3D/rockblock9704-zephyr).
Open issues and pull requests on the GitHub mirror; the Gitea instance does
not allow public sign-ups.

## Ground Control's library

This module does not reimplement or fork the protocol. Ground Control's
repository, [rock7/RockBLOCK-9704](https://github.com/rock7/RockBLOCK-9704)
(MIT), is pinned untouched in `west.yml` at `modules/lib/rockblock9704`,
and `CMakeLists.txt` compiles its portable core. What each part does:

| | Ground Control's library | This module |
| --- | --- | --- |
| JSPR, segmenting, CRC, provisioning check | yes | uses it unchanged |
| Platforms | Linux, Windows, Arduino serial presets | a Zephyr serial port, on the UART in devicetree |
| Threads | none: no locking, shared static buffers | one worker thread owns every call into it |
| API | blocking calls, or async calls with callbacks and `rbPoll()` | requests with a timeout each, callbacks from the worker, a status snapshot |
| Polling | the caller calls `rbPoll()`, one line per call | the worker wakes on a byte from the modem, so an idle modem costs nothing |
| Power pins | `rbBeginGpio` with libgpiod, Linux only | `power-gpios`, `iridium-enable-gpios`, `booted-gpios` in devicetree |
| Python | `rockblock9704` on PyPI, a binding of the C | the `rb9704` command line uses it; the fake modem tests it |

The library's source shows several things a caller has to work around;
`docs/JSPR.md`, section 8, lists them all. The ones this module handles:

- **Provisioning is asked once and kept for good**, a miss included. A
  modem provisioned after the first send would refuse every topic until
  a reboot. Before a send whose topic is not in what the library kept,
  and after the modem refuses one, the module clears the library's cache
  so it asks again: one free JSPR round trip.
- **The blocking calls stop working once callbacks are registered.** The
  module uses the async path only.
- **A query during a send swallows the modem's segment requests**, since
  the reader discards lines it is not waiting for. Info and signal
  requests are refused with `-EBUSY` while a message is moving.
- **No locking.** Every library call is on the worker thread.
- **A send has no timeout, and the queue's count survives `rbEnd`.** The
  async path only reacts to lines from the modem, so a modem that goes
  quiet mid-send leaves the message in flight for good; `rb9704_cancel`
  asks the modem to abandon it. `rbBegin` empties the queue but not the
  count of it, which would make every send after a stuck one queue
  without transmitting; `rb9704_begin` zeroes the count.
- **A line with no status code reads past the library's buffer**, and on
  a board it crashed. A modem talks before `rb9704_begin` opens the
  library: its boot lines, once its power pins bring it up. The module
  listens on the UART from boot and throws those bytes away, so a small
  hardware FIFO never overflows into fragments of old lines, and when the
  library opens part-way through a line the rest of that line is thrown
  away too.

Others are for upstream to fix, and are listed in `docs/JSPR.md` for that:
`setState` waits for a misspelt target, a line with no status code reads
past its buffer (the module keeps the causes it knows of away from it),
MT segment data is cut at 1446 characters, and the Python binding never
releases the GIL.

## Using it

In the application's `west.yml`; the import brings Ground Control's
library, pinned, at the path the module looks in, and nothing else:

```yaml
    - name: rockblock9704-zephyr
      url: https://github.com/SH3D/rockblock9704-zephyr
      revision: v0.3.0
      path: modules/lib/rockblock9704-zephyr
      import:
        name-allowlist:
          - rockblock9704
```

In the board overlay, a node on the modem's UART. The pins are optional;
without them the modem is taken to be powered. I_EN starts and stops the
modem; P_EN is the supercapacitor charge enable and is active low, so
`GPIO_ACTIVE_LOW`, and it can be left unconnected:

```dts
&usart3 {
	current-speed = <230400>;
	status = "okay";

	rockblock {
		compatible = "rock7,rockblock-9704";
		power-gpios = <&gpiob 0 GPIO_ACTIVE_LOW>;           /* P_EN, active low */
		iridium-enable-gpios = <&gpiob 1 GPIO_ACTIVE_HIGH>; /* I_EN */
		booted-gpios = <&gpiob 2 GPIO_ACTIVE_HIGH>;         /* I_BTD */
	};
};
```

**USB-C and the header are the same serial port.** The 9704's USB-C goes
through an FTDI chip to the modem's UART, on the same lines as the
header's TX and RX. With the USB-C cable plugged in, that chip drives the
modem's RX, and a board on the header can hear the modem but not reach
it: `rb9704_begin` fails with `-EIO`, and the computer on USB gets no
answer either. Unplug the USB-C and power the 9704 from the header; a
board's 5 V pin will do, as the modem draws a few hundred mA
(`docs/boards/stamp_c3.md`).

`CONFIG_RB9704` follows the node. Then `<rb9704/rb9704.h>`:

```c
static void mo_done(uint16_t topic, uint16_t id, int result, void *user)
{
	/* result 0: the network has it */
}

static void mt(const struct rb9704_mt *m, void *user)
{
	/* m->data, m->len: copy what you need, it is acknowledged after */
}

static const struct rb9704_callbacks cb = { .mo_done = mo_done, .mt = mt };

rb9704_set_callbacks(&cb);
rb9704_begin(K_SECONDS(30));
rb9704_send(RB9704_TOPIC_RAW, buf, len, K_SECONDS(10));	/* costs airtime */
rb9704_send(RB9704_TOPIC_RAW, buf, len, K_NO_WAIT);	/* queues, never blocks */
rb9704_cancel(K_SECONDS(10));				/* the one in flight; -ECANCELED via mo_done */
```

Every request takes a timeout and waits no longer; with `K_NO_WAIT` a send
queues and returns `-EINPROGRESS`, and its outcome, a refusal included,
comes through `mo_done`. Callbacks run on the worker thread. One modem per
system: the library keeps global state.

| Kconfig | Default | |
| --- | --- | --- |
| `CONFIG_RB9704_MESSAGE_MAX` | 2048 | largest message each way; three static buffers of this size |
| `CONFIG_RB9704_WORKER_STACK_SIZE` | 8192 | measured: the library parks 1.5 kB on the stack in `rbPoll()` |
| `CONFIG_RB9704_WORKER_PRIORITY` | 8 | |
| `CONFIG_RB9704_RX_RING_SIZE` | 8192 | the library's longest JSPR line |
| `CONFIG_RB9704_POWER` | with `iridium-enable-gpios` | drive the pins |
| `CONFIG_RB9704_SHELL` | with `CONFIG_SHELL` | `rb9704` on the shell |
| `CONFIG_RB9704_UPSTREAM_DIR` | empty | where Ground Control's library is, if not where `west.yml` puts it |

## The shell

```
rb9704 begin | end | power on|off
rb9704 status            what is known, without asking the modem
rb9704 info              IMEI, serial, versions, ICCID, SIM, temperature
rb9704 signal            bars now
rb9704 send <topic> <text>  |  sendhex <topic> <hex>     costs airtime
rb9704 cancel            abandon the message in flight
rb9704 spy on|off        every JSPR line, both ways
```

`samples/shell` is the shell plus callbacks that print what arrives.

## Host tools

`python/` is `rb9704` (the modem and Cloudloop from a computer), `rb9704-fake`
(a fake modem on a pty or a serial port), and the tests. `python/README.md`
has every verb; `docs/CLOUDLOOP.md` is the account, the provisioning and
the order to test a new modem in.

## Testing

```sh
python/install.sh
python/.venv/bin/pytest -q python/tests                           # Python
python/.venv/bin/pytest -q python/tests --zephyr-exe <native_sim zephyr.exe>
python/.venv/bin/pytest -q python/tests/test_zephyr.py \
    --board-console /dev/ttyACM0 --board-modem /dev/ttyUSB0       # a board
python/.venv/bin/pytest -q python/tests/test_real.py --real-modem /dev/ttyUSB0
```

`docs/TESTING.md` has the levels, the two-board rig that needs no
USB-serial adapter, and what has passed: the Python level, `native_sim`,
an ESP32-S3 and an STM32U5 against the fake, and free checks against a
real 9704 from Python and from `native_sim`.

## Boards

Any board with a free UART, on any architecture Zephyr runs on. The
library is portable C and the module has nothing tied to one CPU. The
sample has overlays for:

| Board | CPU | Run on |
| --- | --- | --- |
| `native_sim` | the host (x86-64) | yes |
| NUCLEO-U575ZI-Q | STM32U5, Arm Cortex-M33 | yes |
| Seeed XIAO ESP32S3 | ESP32-S3, Xtensa | yes |
| M5Stamp-C3 | ESP32-C3, RISC-V | yes, a real 9704 under the sky: provisioned, a message sent to Cloudloop; `docs/boards/stamp_c3.md` |
| ESP32-S3-DevKitC | ESP32-S3, Xtensa | builds |
| ESP32 DevKitC | ESP32, Xtensa | builds |
| LilyGO T3-S3 | ESP32-S3, Xtensa | builds; an out-of-tree board in OGLAST2, with its `-DBOARD_ROOT` |

On an ESP32, `-DEXTRA_CONF_FILE=overlay-wifi.conf` puts the shell on
telnet as well, for a board out under the sky with no USB
(`docs/TESTING.md`).

## Layout

```
CMakeLists.txt, Kconfig, zephyr/module.yml   the module
dts/bindings/rock7,rockblock-9704.yaml       the devicetree binding
include/rb9704/rb9704.h                      the API
src/                                         serial port, shims, worker, power, shell
samples/shell/                               the shell sample, with board overlays
tests/rig/                                   the two-board test rig
python/                                      rb9704, rb9704-fake, the tests
docs/JSPR.md                                 the protocol as the library speaks it
docs/CLOUDLOOP.md                            account to first message
docs/TESTING.md                              the tests and how to run them
docs/boards/                                 notes for one board each: pins, setup
west.yml                                     the pins
```

## Credits

- [Ground Control](https://www.groundcontrol.com/), for the RockBLOCK 9704
  and its [C library](https://github.com/rock7/RockBLOCK-9704) (MIT),
  which does all of the protocol work. This module builds it unchanged.
- The serial port and the worker began in OGLAS's satellite bench and were
  carried through WII6 before moving here.

## Licence

MIT, as Ground Control's library is. See `LICENSE`.

Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>

* https://www.sh3d.com.au/ - Sh3d
* https://www.dd.com.au/ - Digital Dimensions

## Support

Report issues on the [GitHub mirror](https://github.com/SH3D/rockblock9704-zephyr/issues)
(the Gitea instance does not allow public sign-ups). Most helpful in a
report: the output of `rb9704 spy on` around the problem, and the modem's
firmware version from `rb9704 info`.

## Disclaimer

This is not an official Ground Control product. Use at your own risk, and
remember that every `send` costs airtime.
