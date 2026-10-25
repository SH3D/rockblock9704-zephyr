/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 */
#ifndef SAMPLE_WIFI_H
#define SAMPLE_WIFI_H

#include <stdbool.h>

/* Join the network `wifi cred add` stored, and rejoin it after a drop. */
void sample_wifi_start(void);
bool sample_wifi_connected(void);

#endif /* SAMPLE_WIFI_H */
