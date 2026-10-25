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
| GPIO2 | the RGB LED, an SK6812 | on the board |
| GPIO3 | the button | on the board |
| GPIO20, GPIO21 | `uart0`, the console | through the USB-C bridge |

Kept clear: GPIO8 and GPIO9 (strapping pins, and the board's I2C) and
GPIO18 and GPIO19 (USB). The modem's supply is its own, at the voltage
and peak current Ground Control's manual gives, not the Stamp's 3.3 V.

The control lines are optional. With fewer wired, take the matching lines
out of the overlay's `rockblock` node, or `rb9704 begin` waits on a pin
that is not there:

- None: four wires, and the modem is taken to be powered.
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
| the LED and button | built, not yet run |
| a real 9704 on this board | not yet |
