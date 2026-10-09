/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rb9704_glue.h - what Ground Control's library needs declared on Zephyr.
 *
 * Force-included into every library file (CMakeLists.txt): none of its
 * platform branches (__linux__, _WIN32, ARDUINO) is taken on Zephyr, so its
 * calls to the serial-setup hook and to millis() and delay() would otherwise
 * be implicit declarations. rb9704_serial.c and rb9704_shims.c define them.
 * usleep() is not declared here: picolibc's <unistd.h> does.
 */
#ifndef RB9704_GLUE_H_
#define RB9704_GLUE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SERIAL_CONTEXT_SETUP_FUNC: fills the library's serialContext with this
 * port. `port` is ignored; the UART is the devicetree node's bus. */
bool rb9704_serial_set_context(const char *port, uint32_t baud);

unsigned long millis(void);
void delay(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* RB9704_GLUE_H_ */
