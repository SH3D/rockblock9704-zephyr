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

| Stamp-C3 | 9704 header | Notes |
| --- | --- | --- |
| GPIO4, `uart1` TX | 14, RXD | 230400 8N1 |
| GPIO5, `uart1` RX | 13, TXD | |
| GPIO6 | 3, I_EN | starts and stops the modem |
| GPIO7 | 7, I_BTD | high once booted, low once shut down; waited on both ways |
| GPIO10 | 6, P_EN | the supercapacitor charge enable, **active low**; optional |
| GND | 1, 4 or 10, GND | signal ground |
| | 15, V_IN+ and 16, V_IN- | 4.0 to 5.3 V, 500 mA: see Power |
| GPIO2 | | the RGB LED, an SK6812, on the board |
| GPIO3 | the button | on the board |
| GPIO20, GPIO21 | `uart0`, the console | through the USB-C bridge |

Kept clear: GPIO8 and GPIO9 (strapping pins, and the board's I2C) and
GPIO18 and GPIO19 (USB).

## Power: V_IN+, and the 9704's USB-C unplugged

The 9704's header UART and its USB-C port are the same serial lines. With
USB-C plugged in, the 9704's FTDI chip is powered and drives the modem's
RX, and a board on the header cannot drive the line, so the modem gets
no request from either side, the computer on USB included. Its TX still
reaches the header, so a board can receive alongside USB, but not send.

So the 9704 is powered from the header with its USB-C unplugged, on
V_IN+ (pin 15) and V_IN- (pin 16): 4.0 to 5.3 V, and the board limits
itself to about 460 mA while its supercapacitor charges, about 40 s to
the green RDY LED. A USB supply of its own is best, with its ground
joined to the Stamp's; the Stamp's 5V pin works too, sharing the Stamp's
USB-C. V_BATT (pin 12) is for a single lithium cell, 3.6 to 4.5 V, with
no reverse-polarity protection and no charger on the board: never 5 V
there. Ground Control recommends one input, not both.

The control lines are optional. With fewer wired, take the matching lines
out of the overlay's `rockblock` node, or `rb9704 begin` waits on a pin
that is not there:

- None: four wires, and the modem is taken to be powered: I_EN has a
  pull-up and P_EN a pull-down, so it boots and charges when power comes.
  No edit needed: `-DEXTRA_DTC_OVERLAY_FILE=no-control-pins.overlay`
  takes all three out at build time (below).
- I_EN: `rb9704 power off` and `begin` over telnet restart a modem that
  has hung, with nobody walking out to it. It is the line most worth
  wiring.
- I_BTD as well: start-up and shutdown wait for I_BTD to follow I_EN,
  as Ground Control's page asks, not a fixed 4 s
  (`boot-delay-ms`). Moving I_EN back before I_BTD has followed "may
  result in damage to the 9704 module".
- P_EN adds nothing for a modem on V_IN+: left open, the board's
  pull-down keeps the charger on. Wired, it must be `GPIO_ACTIVE_LOW`;
  driven high it switches the charger off.

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

With no I_EN, the long press on the button ends without turning anything
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
| 2 s or more | end, and the modem off when I_EN is wired |

Nothing on the button costs airtime. The LED runs at 24 of 255
(`LEVEL` in `indicator.c`).

## First power-up

1. Without the modem: the LED blinks blue until WiFi joins, then stays
   blue. A short press runs `begin` with nothing there: white, then a red
   flash.
2. With the modem wired: a short press goes white, then red, yellow or
   green with the signal.
3. Over telnet: `rb9704 spy on`, `rb9704 info`, `rb9704 signal`. None
   of the three costs airtime.

A fake modem (`rb9704-fake`) on a USB-serial adapter in the modem's place
has no I_BTD: tie GPIO7 to 3.3 V for those runs.

## What has run

| | |
| --- | --- |
| WiFi, the stored network, telnet | works (2026-10-24) |
| a real 9704 on TX and RX, on V_IN+ from the Stamp's 5V: `begin`, `info`, `signal` | works (2026-10-25) |
| the same with the 9704's USB-C plugged in | fails: the FTDI holds the modem's RX |
| P_EN, I_EN, I_BTD: `begin` powers it up in about 2 s, `power off`, `begin` again | works (2026-10-25), 8 of 8 cycles, and 12 of 12 with up to 5 s between `power on` and `begin` |
| under open sky: the constellation visible and 3 to 5 bars within seconds of `begin` | works (2026-10-25) |
| staying up while active | failed with P_EN driven the wrong way: see below; with P_EN unconnected, up and answering throughout (2026-10-25) |
| provisioning, and a message sent | works (2026-10-25): provisioned with 244 and 313 to 317, and "Stamp-C3 says hi", 16 bytes on topic 244, sent in 10 s with 3 to 4 bars and received in Cloudloop |
| the reworked control pins outside: I_EN and I_BTD in Ground Control's sequence, P_EN wired and `GPIO_ACTIVE_LOW` | works (2026-10-25): "Stamp-C3 P_EN low", 17 bytes on topic 244, sent in 6 s and received in Cloudloop, and the modem up overnight; see below |
| an MT message down to the board | not yet |
| the LED and button | the button's short press works; the busy colour reads orange, not white, at this brightness |

### The modem stopped answering after a few minutes: P_EN

On 2026-10-25 the modem stopped answering 2 to 5 minutes after each
`begin`, four times: once indoors, three times under open sky. Every
request then got `-EIO`, its signal reports stopped, and it stayed silent
until the control lines were cycled. Indoors the RDY LED went off.

The overlay had P_EN as `GPIO_ACTIVE_HIGH`. P_EN is active low (Ground
Control's hardware page, pin 6, "Cap Charge Enable Signal (ACTIVE LOW)"),
so every `begin` drove it high and switched the supercapacitor charger
off: the modem ran on what the supercapacitor held until it was flat.
The TX and RX test without control lines, where the pull-down keeps the
charger on, did not stop. With P_EN taken off the header, the same
firmware stayed up and sent its first message. The overlay now has
`GPIO_ACTIVE_LOW`, and the module releases P_EN while the modem is off;
with P_EN wired again on that firmware, the modem sent a second message
and stayed up through the night, about 11 h 54 min from 22:41 on
2026-10-25 to 10:35 the next morning, satellites seen throughout.

The module also now shuts down as the page sets out: I_EN low, then wait
for I_BTD to go low before anything else, and the same wait on the way
up. Before, `power off` dropped the pins and returned at once.
