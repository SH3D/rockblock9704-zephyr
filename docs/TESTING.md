# Testing

One fake modem, three places to run the code against it, and a few free
checks against a real modem.

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
`/dev/ttyUSB0`, which is how host Zephyr talks to real hardware.

## The rig: two boards, one wire

No USB-serial adapter to hand? Two boards with a UART wired between them
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

Not yet: a message through a real modem and satellite, which costs
airtime and needs the sky (`CLOUDLOOP.md`, "The order to test in"), and
the power pins on a board that has them wired.
