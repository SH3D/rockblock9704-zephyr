# Changes

## Unreleased

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
