# Changes

## Unreleased

- `rbtest` in the shell sample: airtime tests with a payload that can be
  checked at the other end. `rbtest small` sends a few 32-byte messages
  one after another and logs how long each took, `rbtest large` sends one
  message of up to 50000 bytes and sends nothing without `confirm`, and
  `rbtest mt` shows the last MT and checks it if it is a test message.
  `rb9704 pattern make` and `check` in `python/` make and check the same
  messages. Not yet run against a real modem.
- `rb9704_send_fill`: as `rb9704_send`, with the message written straight
  into the module's buffer, so a large generated message is not held
  twice. `overlay-large.conf` raises the limit to 50000 bytes; on the
  M5Stamp-C3 that fits on the USB console and not beside WiFi
  (`docs/boards/stamp_c3.md`).

- Docs: the M5Stamp-C3 left out overnight on 0.3.0, P_EN wired and
  active low: the modem up and answering for about 11 h 54 min with no
  drop, and the Stamp did not reboot. P_EN driven the wrong way had
  stopped it within 5 minutes every time.

## 0.3.0, 2026-10-25

- Provisioning and MO messages to Cloudloop work: the M5Stamp-C3 under
  open sky, provisioned with topics 244 and 313 to 317, sent two
  messages on topic 244 from its shell over WiFi, 16 and 17 bytes, and
  both arrived in Cloudloop whole. The second ran on the reworked
  control pins below, with P_EN wired and active low, and the modem
  stayed up past every earlier drop. `docs/CLOUDLOOP.md`, "What has
  run", records how Cloudloop shows a message.

- P_EN is active low. On the RockBLOCK it is the supercapacitor charge
  enable ("Cap Charge Enable Signal (ACTIVE LOW)", header pin 6), and the
  sample's overlays had it `GPIO_ACTIVE_HIGH`, so every `begin` switched
  the charger off: the modem ran down and went silent 2 to 5 minutes
  later, as it did on the M5Stamp-C3 indoors and out. The overlays, the
  README and the binding now say `GPIO_ACTIVE_LOW`, and the module drives
  P_EN only while the modem runs, releasing it to an input while it is
  off, where the board's pull-down keeps the charger on.
- The control pins follow Ground Control's start-up and shutdown
  sequence. I_EN starts and stops the modem, and is what makes the pins
  present (`CONFIG_RB9704_POWER` follows `iridium-enable-gpios`); P_EN
  and I_BTD are optional beside it. Shutdown drives I_EN low and waits
  for I_BTD to go low before anything else, as start-up waits for it to
  go high, so I_EN never moves back before I_BTD has followed, which the
  page warns may damage the module. Before, `power off` dropped the pins
  and returned at once.

- The modem's UART is listened to from boot, not only while the library
  is open. With the power pins wired, `rb9704 power on` or a modem left
  talking after `rb9704 end` sent its boot lines to nobody; they
  overflowed the M5Stamp-C3's 128-byte UART FIFO, after which `begin`
  heard only repeated fragments of old lines and failed with `-EIO` (1 in
  6 after a power cycle, none at all after a pause), and a second `begin`
  crashed the board in the library's line parser. Bytes heard while the
  library is closed are now thrown away, and when it opens part-way
  through a line the rest of that line is too. On the Stamp: 8 of 8 power
  cycles, and 12 of 12 with 0 to 5 s between power on and `begin`.
- The fake modem's `raw` and `before` write any bytes, now or just ahead
  of its next reply. `test_a_boot_burst_while_closed` sends a 680-byte
  boot burst before `begin`: the overflow above on a board through the
  rig, a check that it does no harm on native_sim.

## 0.2.0, 2026-10-25

- The shell sample over WiFi: `overlay-wifi.conf` puts the shell on
  telnet on any ESP32, joining the network `wifi cred add` stored in
  flash at boot and rejoining it after a drop.
- The shell sample on the M5Stamp-C3 (ESP32-C3, RISC-V): the modem on
  `uart1`, GPIO4 and GPIO5. Its RGB LED shows the modem's state and its
  button begins, asks for the signal and ends. `docs/boards/stamp_c3.md`
  has its pins, the WiFi commands and the LED's colours.
- The sample's overlays for the M5Stamp-C3, ESP32-S3-DevKitC, ESP32
  DevKitC and NUCLEO-U575ZI-Q wire P_EN, I_EN and I_BTD; the wiring table
  is in `docs/TESTING.md`. `no-control-pins.overlay` takes them out at
  build time, for a modem on TX and RX alone; the modem node is labelled
  `rb9704` in every overlay so it can.
- A real 9704 on a microcontroller for the first time: the M5Stamp-C3,
  on TX and RX, powered from the Stamp's 5 V. The 9704's USB-C and its
  header share the modem's UART, and with USB-C plugged in a board on the
  header cannot reach the modem; the README, `docs/CLOUDLOOP.md` and
  `docs/boards/stamp_c3.md` say so.
- `rb9704_cancel()` and `rb9704 cancel`: ask the modem to abandon the
  message in flight; `mo_done` then reports `-ECANCELED`. The status
  snapshot carries the in-flight message's id and start time. The fake
  modem's `mo-hold` and `mo-release` stand in for a modem that has taken
  a message and gone quiet.
- `rb9704_begin` zeroes the library's count of queued MOs, which `rbBegin`
  leaves behind and which would otherwise stop every send after a stuck
  one.
- `rb9704 send`: no fourth `CONFIG_RB9704_MESSAGE_MAX` buffer; text goes
  straight through, hex into a buffer the size of a shell line.
- `rb9704`: a Cloudloop error, a bad token's `{"error": ...}` in a 200
  reply included, and a port that will not open are one line and a
  non-zero exit, not a traceback or a quiet success; a one-word `jspr`
  line is refused instead of raising.
- `CONFIG_RB9704_POWER` now defaults on when the devicetree node has
  `power-gpios`, as documented. The Kconfig default referred to a compat
  variable nothing defined, so the pins were never driven unless the
  option was set by hand.
- A request whose own result was `-ETIMEDOUT`, such as a begin whose modem
  never signalled booted, lost the request slot for good and every later
  call returned `-EAGAIN`. The caller's abandonment of a request is now
  reported apart from the request's result.
- When the booted pin does not rise in time, the supply and enable pins are
  dropped again, so the next attempt starts from off.
- `rb9704`: paths in its messages that belonged to the tree it came from.

## 0.1.0, 2026-10-09

The first release.

- Ground Control's RockBLOCK 9704 library v1.0.1, pinned untouched, built
  as a Zephyr module with a serial port on the devicetree node's UART.
- One worker thread owns the library; it sleeps until a request or a byte
  from the modem arrives.
- `<rb9704/rb9704.h>`: begin, end, power, send, info, signal, each with a
  timeout; callbacks for a sent message, a received one, the signal and
  the provisioning; a status snapshot.
- Provisioning asked again when the library's cached answer is missing a
  topic, or after the modem refuses one.
- The `rock7,rockblock-9704` binding with optional power, enable and
  booted pins.
- The `rb9704` shell command and the shell sample, with overlays for
  native_sim, NUCLEO-U575ZI-Q, XIAO ESP32S3 and ESP32-S3-DevKitC.
- `python/`: the `rb9704` command line, the `rb9704-fake` fake modem, and
  tests for the Python binding, native_sim, boards and a real modem.
