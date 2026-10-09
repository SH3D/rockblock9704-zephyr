/* SPDX-License-Identifier: MIT
 *
 * rb9704_shims.c - the library's three timing calls on the Zephyr kernel,
 * in place of its crossplatform.c, which has no Zephyr branch.
 */
/* useconds_t and usleep() sit behind a feature macro in picolibc. */
#define _DEFAULT_SOURCE
#include <zephyr/kernel.h>
#include <unistd.h>

#include "rb9704_glue.h"

unsigned long millis(void)
{
	return k_uptime_get_32();
}

void delay(uint32_t ms)
{
	k_msleep(ms);
}

/* picolibc declares usleep in <unistd.h>; the library calls it directly. */
int usleep(useconds_t us)
{
	k_usleep(us);
	return 0;
}
