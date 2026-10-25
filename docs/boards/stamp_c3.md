# M5Stamp-C3

M5Stack's Stamp-C3: an ESP32-C3, a single RISC-V core with WiFi and
400 kB of RAM, on a small board with a USB-C serial bridge, an RGB LED
and a button. Zephyr's name for it is `stamp_c3`, and it is in Zephyr
upstream. It is the board for taking the modem outside: powered from a
battery or a supply, reached over WiFi, and showing on its LED how things
stand.

The overlay is `samples/shell/boards/stamp_c3.overlay`, with
`stamp_c3.conf` beside it for the LED and button.

## Pins

| Stamp-C3 | Signal | Notes |
| --- | --- | --- |
| GPIO4 | `uart1` TX → 9704 RX | 230400 8N1 |
| GPIO5 | `uart1` RX ← 9704 TX | |
| GPIO10 | P_EN | the supply; without it the module drives none of the three |
| GPIO6 | I_EN | the modem enable, 10 ms after P_EN |
| GPIO7 | I_BTD | `rb9704 begin` waits up to 30 s for it to go high |
| GND | GND | |
| 5V | the 9704's supply in | the Stamp's USB-C supply, passed through |
| GPIO2 | the RGB LED, an SK6812 | on the board |
| GPIO3 | the button | on the board |
| GPIO20, GPIO21 | `uart0`, the console | through the USB-C bridge |

Kept clear: GPIO8 and GPIO9 (strapping pins, and the board's I2C) and
GPIO18 and GPIO19 (USB).

## Power: the Stamp's 5V, and the 9704's USB-C unplugged

The 9704's header UART and its USB-C port are the same serial lines. With
USB-C plugged in, the 9704's FTDI chip is powered and drives the modem's
RX, and a board on the header cannot talk over it: the modem answers
nobody, the computer on USB included. Its TX still reaches the header, so
a board can listen alongside USB, but not send.

So the 9704 is powered from the header with its USB-C unplugged. The
Stamp's 5V pin does it: the modem draws a few hundred mA, and its
supercapacitor charges slowly, about 40 s to the green RDY LED. Both
boards then run from the Stamp's USB-C; on a laptop's 500 mA port a
Stamp that resets by itself while the 9704 charges is the supply sagging,
and a charger or a powered hub gives the margin.

The control lines are optional. With fewer wired, take the matching lines
out of the overlay's `rockblock` node, or `rb9704 begin` waits on a pin
that is not there:

- None: four wires, and the modem is taken to be powered. No edit
  needed: `-DEXTRA_DTC_OVERLAY_FILE=no-control-pins.overlay` takes all
  three out at build time (below).
- P_EN only: `rb9704 power off` and `power on` over telnet restart a
  modem that has hung, with nobody walking out to it. The one most worth
  wiring.
- I_BTD as well: a slow or failed boot is a clear timeout, not JSPR
  errors after a fixed 4 s (`boot-delay-ms`).
- I_EN adds little here. The ESP32-C3's ROM prints at reset on GPIO21,
  the console, so nothing reaches the modem's pins before Zephyr runs.

## Build and flash

From a west workspace that has this module:

```sh
west build -b stamp_c3 samples/shell -- -DEXTRA_CONF_FILE=overlay-wifi.conf
west flash
```

Without `-DEXTRA_CONF_FILE=overlay-wifi.conf` the shell is on the USB
console only, and the LED and button work the same.

TX and RX only, no control lines:

```sh
west build -b stamp_c3 samples/shell -- -DEXTRA_CONF_FILE=overlay-wifi.conf \
    -DEXTRA_DTC_OVERLAY_FILE=no-control-pins.overlay
```

With no P_EN, the long press on the button ends without turning anything
off, and `rb9704 power` answers `-ENOTSUP`.

## WiFi

Once, on the USB console (115200), store the network. It is kept in flash
across reboots and reflashing:

```
wifi cred add -s "<SSID>" -k 1 -p "<password>"
```

`-k 1` is WPA2-PSK; `-k 0` is an open network. The board joins at every
boot after, tries again every 30 s while the network is away, and logs:

```
address 192.168.1.50: telnet 192.168.1.50
```

| Command | |
| --- | --- |
| `wifi cred list` | the stored networks |
| `wifi cred delete <SSID>` | forget one |
| `wifi cred auto_connect` | join a stored network now |
| `wifi status` | the network joined, the channel, the signal |
| `wifi scan` | the networks in range |
| `net iface` | the address, again |

## Telnet

```sh
telnet 192.168.1.50
```

Zephyr's telnet server does not offer character mode, so the client stays
in line mode, and the up arrow, Tab and line editing do nothing. Ask for
it: `Ctrl-]`, then `mode character`. For every session, `~/.telnetrc`:

```
192.168.1.50
    mode character
```

Or raw, with every key going to the shell and `Ctrl-]` to leave:

```sh
socat -,rawer,escape=0x1d TCP:192.168.1.50:23
```

Telnet has no password and no encryption, and `rb9704 send` costs
airtime: use it on a network you trust.

## The LED and button

`samples/shell/src/indicator.c`.

| LED | Meaning |
| --- | --- |
| blue | not begun |
| red / yellow / green | begun: no signal or no satellite / 1-2 bars / 3-5 bars |
| magenta | a message is with the modem, on its way |
| green flash, 1 s | a message went |
| red flash, 2 s | a message, or a button action, failed |
| cyan flash, 2 s | a message arrived |
| white, fast blink | a button action is running |
| slow blink of any of the above | no WiFi: telnet will not reach it |

| Button | |
| --- | --- |
| short press | not begun: `rb9704 begin`. Begun: ask for the signal |
| 2 s or more | end, and the supply off when P_EN is wired |

Nothing on the button costs airtime. The LED runs at 24 of 255
(`LEVEL` in `indicator.c`).

## First power-up

1. Without the modem: the LED blinks blue until WiFi joins, then stays
   blue. A short press runs `begin` with nothing there: white, then a red
   flash.
2. With the modem wired: a short press goes white, then red, yellow or
   green with the signal.
3. Over telnet: `rb9704 spy on`, `rb9704 info`, `rb9704 signal`. All
   free.

A fake modem (`rb9704-fake`) on a USB-serial adapter in the modem's place
has no I_BTD: tie GPIO7 to 3.3 V for those runs.

## What has run

| | |
| --- | --- |
| WiFi, the stored network, telnet | works (2026-10-24) |
| a real 9704 on TX and RX, powered from the Stamp's 5V: `begin`, `info`, `signal` | works (2026-10-25) |
| the same with the 9704's USB-C plugged in | fails: the FTDI holds the modem's RX |
| P_EN, I_EN, I_BTD: `begin` powers it up in about 2 s, `power off`, `begin` again | works (2026-10-25), 8 of 8 cycles, and 12 of 12 with up to 5 s between `power on` and `begin` |
| under open sky: the constellation visible and 3 to 5 bars within seconds of `begin` | works (2026-10-25) |
| staying up while active, on the Stamp's 5V | fails: see below |
| provisioning, and a message sent | not yet |
| the LED and button | the button's short press works; the busy colour reads orange, not white, at this brightness |

### Open: the modem stops answering after a few minutes

On 2026-10-25, powered from the Stamp's 5V, the modem stopped answering
2 to 5 minutes after each `begin`, four times: once indoors, three times
under open sky. Every request then gets `-EIO`, its signal reports stop,
and it stays silent until the control lines are cycled; left alone with
P_EN and I_EN high it was still silent after 9 minutes. Indoors the RDY
LED went off when it happened.

The likely cause is the supply: the 9704's input going below the 4.0 V
it needs while it charges, after the Stamp's USB-C, its 5V pin and the
leads. That is not yet measured. What would settle it: the voltage at
the 9704's supply pin over those minutes; the 9704 on a 5 V supply of
its own, GND joined to the Stamp's; and the 9704 on its own USB-C from a
laptop for 30 minutes, which has not been tried for longer than a few
seconds either. Whether the modem goes quiet on its serial port while it
provisions is also not known, and a test that power-cycles it on
silence would cut that short.
