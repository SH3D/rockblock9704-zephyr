# Changes

## Unreleased

- The shell sample over WiFi: `overlay-wifi.conf` puts the shell on
  telnet on any ESP32, joining the network `wifi cred add` stored in
  flash at boot and rejoining it after a drop.
- The shell sample on the M5Stamp-C3 (ESP32-C3, RISC-V): the modem on
  `uart1`, GPIO4 and GPIO5. Its RGB LED shows the modem's state and its
  button begins, asks for the signal and ends. `docs/boards/stamp_c3.md`
  has its pins, the WiFi commands and the LED's colours.
- The sample's overlays for the M5Stamp-C3, ESP32-S3-DevKitC, ESP32
  DevKitC and NUCLEO-U575ZI-Q wire P_EN, I_EN and I_BTD; the wiring table
  is in `docs/TESTING.md`.
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
