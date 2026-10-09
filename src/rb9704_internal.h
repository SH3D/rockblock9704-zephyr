/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rb9704_internal.h - between this module's own files.
 */
#ifndef RB9704_INTERNAL_H_
#define RB9704_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#define RB9704_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(rock7_rockblock_9704)

/* rb9704_serial.c */
int rb9704_serial_available(void);
uint32_t rb9704_serial_dropped(void);
void rb9704_serial_spy(bool on);

/* rb9704_worker.c */
k_tid_t rb9704_worker_tid_get(void);
void rb9704_worker_rx(void);

/* rb9704_power.c, or the stubs in rb9704_worker.c without the pins */
int rb9704_power_init(void);
int rb9704_power_set(bool on);
bool rb9704_power_on(void);
bool rb9704_power_present(void);

#endif /* RB9704_INTERNAL_H_ */
