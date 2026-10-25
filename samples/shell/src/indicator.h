/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 */
#ifndef SAMPLE_INDICATOR_H
#define SAMPLE_INDICATOR_H

/* The RGB LED shows the modem's state; the button begins and ends it. */
void indicator_start(void);
/* A short flash for a finished send (result as mo_done gives it) and for
 * a message arrived. */
void indicator_flash_mo(int result);
void indicator_flash_mt(void);

#endif /* SAMPLE_INDICATOR_H */
