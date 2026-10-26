# Testing

The tests run one fake modem against the code in three places, and a few
free checks against a real modem.

| Level | What runs | Against | Needs |
| --- | --- | --- | --- |
| Python | Ground Control's Python binding, the `rb9704` command line, the fake itself | the fake on a pty | Python 3.10 to 3.14 |
| Host Zephyr | this module in the shell sample on `native_sim` | the fake on a pty, or a real modem on USB | a Zephyr workspace |
| Board | the same sample on an ESP32-S3, STM32U5 or any board | the fake on a USB-serial port wired to the board's modem pins | a board and a wire |

The native_sim and board runs are the same tests (`python/tests/test_zephyr.py`)
through a different console, so a behaviour seen on one is checked on the
other.

## The fake modem

`python/rb9704tools/fakemodem.py`, `rb9704-fake` on the command line. It
implements JSPR as `JSPR.md` describes it: the three exchanges `rbBegin`
needs, the identity and signal getters, provisioning, MO in segments of
any size with the CRC checked, MT in segments the library can take, and
on command a refusal, a failed MO, a signal change or a change of
provisioning. It writes every whole MO it receives as an event line, so a
test checks the bytes that reached "the network".

What it cannot tell you is what a real modem does that the library's
source does not show: unsolicited lines at odd moments, timing, MT
segment sizes. That is what `test_real.py` and the hardware are for.

## Running them

```sh
python/install.sh

# Python level: the fake, the command line, Ground Control's binding
python/.venv/bin/pytest -q python/tests

# Host Zephyr: build the sample for native_sim, then
west build -b native_sim/native/64 samples/shell -d build/native
python/.venv/bin/pytest -q python/tests --zephyr-exe build/native/zephyr/zephyr.exe

# Board: flash the sample with its modem UART wired to a USB-serial
# adapter (3.3 V), then the board's shell port and the adapter's port
python/.venv/bin/pytest -q python/tests/test_zephyr.py \
    --board-console /dev/ttyACM0 --board-modem /dev/ttyUSB0

# A real modem, free checks only (identity and signal, never a message)
python/.venv/bin/pytest -q python/tests/test_real.py --real-modem /dev/ttyUSB0 \
    [--zephyr-exe build/native/zephyr/zephyr.exe]
```

Every option has an environment variable as well: `RB9704_ZEPHYR_EXE`,
`RB9704_BOARD_CONSOLE`, `RB9704_BOARD_MODEM`, `RB9704_REAL_MODEM`.

On native_sim the modem UART is a `zephyr,native-tty-uart` node
(`samples/shell/boards/native_sim.overlay`); the tests give it the fake's
pty with `-modem_uart_port=`. The same option takes a real modem's
`/dev/ttyUSB0`, which is how host Zephyr reaches real hardware.

## The rig: two boards, one wire

Without a USB-serial adapter, two boards with a UART wired between them
will do: one runs the sample, the other runs `tests/rig/bridge`, which
copies bytes between that UART and its own USB serial port, where the
fake answers.

```
host --USB-- bridge board --UART-- board under test --USB-- host
             (tests/rig/bridge)    (samples/shell)
  fake modem on this port                       pytest drives this shell
```

The overlays in `tests/rig/` are for a NUCLEO-U575ZI-Q (USART2 on PD5 and
PD6) and a Seeed XIAO ESP32S3 (D8 and D9) wired to each other, either way
round:

| Under test | Bridge | `--board-console` | `--board-modem` |
| --- | --- | --- | --- |
| XIAO ESP32S3, `-DDTC_OVERLAY_FILE=tests/rig/xiao_esp32s3_rig.overlay` | Nucleo, `tests/rig/bridge` | the XIAO's USB | the Nucleo's ST-LINK port |
| Nucleo, `-DDTC_OVERLAY_FILE=tests/rig/nucleo_u575zi_q_rig.overlay` | XIAO, `tests/rig/bridge` | the Nucleo's ST-LINK port | the XIAO's USB |

The ESP32-S3 sits in its ROM loader after `west flash` until it is reset;
toggle RTS on its USB port, or press reset, before the tests.

## Out of doors: the shell over WiFi

A real modem needs a view of the sky, which is usually further than a
USB cable goes. `samples/shell/overlay-wifi.conf` puts the same shell on telnet,
on any ESP32 board, so the board needs only power:

```sh
west build -b stamp_c3 samples/shell -- -DEXTRA_CONF_FILE=overlay-wifi.conf
west flash
```

Once, on the USB console, store the network; it is kept in flash:

```
wifi cred add -s "<SSID>" -k 1 -p "<password>"
```

At every boot after, the board joins it, logs `address 192.168.1.50:
telnet 192.168.1.50`, and rejoins after a drop. A network added after boot
is joined within 30 s. `wifi cred list` and `wifi cred delete <SSID>` look
after the stored list; `net iface` shows the address again.

```sh
telnet 192.168.1.50
rb9704 begin
rb9704 spy on
rb9704 info
```

Telnet has no password and no encryption, and `rb9704 send` costs
airtime: use it on a network you trust.

The Linux `telnet` client stays in line mode, where the up arrow and line
editing do nothing: `Ctrl-]` then `mode character` fixes it, or connect
with `socat -,rawer,escape=0x1d TCP:<address>:23`.

On the M5Stamp-C3 the board's RGB LED shows the modem's state and its
button begins and ends it; `boards/stamp_c3.md` has the table, the pins
and the WiFi commands.

### Wiring

The board overlays for the M5Stamp-C3, ESP32-S3-DevKitC, ESP32 DevKitC
and NUCLEO-U575ZI-Q wire all three control lines, so `rb9704 begin`
starts the modem through I_EN and waits for I_BTD to go high, and
`rb9704 power off` waits for I_BTD to go low.

| 9704 header | M5Stamp-C3 | ESP32-S3-DevKitC | ESP32 DevKitC | NUCLEO-U575ZI-Q |
| --- | --- | --- | --- | --- |
| 14, RXD (board TX) | GPIO4 | GPIO17 | GPIO17 | PD8 |
| 13, TXD (board RX) | GPIO5 | GPIO18 | GPIO16 | PD9 |
| 3, I_EN | GPIO6 | GPIO4 | GPIO25 | PF14 (D4) |
| 7, I_BTD | GPIO7 | GPIO5 | GPIO26 | PF13 (D7) |
| 6, P_EN, active low, optional | GPIO10 | GPIO6 | GPIO27 | PF15 (D2) |
| 1, 4 or 10, GND | GND | GND | GND | GND |

The modem's supply goes to V_IN+ and V_IN- (pins 15 and 16), 4.0 to
5.3 V at up to 500 mA, not the board's 3.3 V regulator; V_BATT (pin 12)
is for a lithium cell, never 5 V. P_EN is the supercapacitor charge
enable and is active low: driven high it switches the charger off, and
the modem runs down within minutes. Leaving it unconnected is fine.

A fake modem on a USB-serial adapter in place of the 9704 has no I_BTD:
build with `no-control-pins.overlay`, below, or with the rig overlays,
which have no control lines. Tying I_BTD to 3.3 V gets `begin` going, but
`power off` then waits 30 s for it to fall and fails.

For a modem on TX and RX alone, `samples/shell/no-control-pins.overlay`
takes the three lines out of any board's overlay at build time:
`-DEXTRA_DTC_OVERLAY_FILE=no-control-pins.overlay`.

## What has passed

On 2026-10-09, Zephyr v4.4.0, Ground Control's library v1.0.1, the
binding 1.0.2:

| Level | Tests | Result |
| --- | --- | --- |
| Python: fake, command line, binding | 16 | all pass |
| `native_sim/native/64` against the fake | 12 | all pass |
| XIAO ESP32S3 through the rig | 12 | all pass |
| NUCLEO-U575ZI-Q through the rig | 12 | all pass |
| a real 9704 (firmware v1.5.14), binding and native_sim, free checks | 2 | all pass |

On 2026-10-25, the M5Stamp-C3 (ESP32-C3, RISC-V) with the shell over
WiFi and a real 9704 on its header pins, under open sky: `begin`, `info`
and `signal` answer, the modem was provisioned, two messages on topic
244 were received in Cloudloop, and the modem stayed up overnight with
all three control lines wired. Those were the first messages through a
real modem and a satellite. `boards/stamp_c3.md` has the pins, the runs
and the P_EN fault found on the way.

Not yet: an MT message down to the board.
