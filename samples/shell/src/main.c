/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * The rb9704 shell sample: `rb9704` on the console, and every message from
 * the network, every finished send and every signal change printed as it
 * happens. The same image is what the tests in tests/ drive, on native_sim
 * against a fake modem and on a board against a fake or a real one.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <ctype.h>

#include <zephyr/sys/crc.h>

#include <rb9704/rb9704.h>

#ifdef CONFIG_WIFI
#include "wifi.h"
#endif
#ifdef CONFIG_LED_STRIP
#include "indicator.h"
#endif

LOG_MODULE_REGISTER(sample, LOG_LEVEL_INF);

static void on_mo_done(uint16_t topic, uint16_t id, int result, void *user)
{
	ARG_UNUSED(user);
	LOG_INF("mo done topic %u id %u result %d", topic, id, result);
#ifdef CONFIG_LED_STRIP
	indicator_flash_mo(result);
#endif
}

static void on_mt(const struct rb9704_mt *mt, void *user)
{
	/* The whole message's CRC and its start: a long one in a single log
	 * line would not survive every board's log buffer. */
	char text[65];
	size_t n = MIN(mt->len, sizeof(text) - 1);

	ARG_UNUSED(user);
	for (size_t i = 0; i < n; i++) {
		text[i] = isprint(mt->data[i]) ? mt->data[i] : '.';
	}
	text[n] = '\0';
	LOG_INF("mt topic %u id %u len %u crc %08x: %s", mt->topic, mt->id, (unsigned int)mt->len,
		crc32_ieee(mt->data, mt->len), text);
#ifdef CONFIG_LED_STRIP
	indicator_flash_mt();
#endif
}

static void on_signal(uint8_t bars, bool visible, void *user)
{
	ARG_UNUSED(user);
	LOG_INF("signal %u bars%s", bars, visible ? "" : ", not visible");
}

static void on_provisioning(const struct rb9704_topic *topics, uint8_t n, void *user)
{
	ARG_UNUSED(user);
	LOG_INF("provisioned, %u topic(s), the first %u", n, n > 0 ? topics[0].id : 0);
}

static const struct rb9704_callbacks cb = {
	.mo_done = on_mo_done,
	.mt = on_mt,
	.signal = on_signal,
	.provisioning = on_provisioning,
};

int main(void)
{
	rb9704_set_callbacks(&cb);
#ifdef CONFIG_WIFI
	sample_wifi_start();
#endif
#ifdef CONFIG_LED_STRIP
	indicator_start();
#endif
	LOG_INF("rb9704 shell sample: `rb9704 begin`, then `rb9704 info`");
	return 0;
}
