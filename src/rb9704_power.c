/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rb9704_power.c - the board's control pins, as the devicetree gives them.
 *
 *   power-gpios           P_EN, the supply
 *   iridium-enable-gpios  I_EN, the modem, once the supply is up
 *   booted-gpios          I_BTD, the modem saying it has booted
 *
 * Up: supply, a moment, enable, then the booted pin or boot-delay-ms.
 * Down: enable, then supply. Called on the worker thread only.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <errno.h>

#include "rb9704_internal.h"

LOG_MODULE_DECLARE(rb9704, CONFIG_RB9704_LOG_LEVEL);

#define BOOT_TIMEOUT_MS 30000

static const struct gpio_dt_spec pwr = GPIO_DT_SPEC_GET_OR(RB9704_NODE, power_gpios, {0});
static const struct gpio_dt_spec en = GPIO_DT_SPEC_GET_OR(RB9704_NODE, iridium_enable_gpios, {0});
static const struct gpio_dt_spec btd = GPIO_DT_SPEC_GET_OR(RB9704_NODE, booted_gpios, {0});

static bool on;

bool rb9704_power_present(void)
{
	return pwr.port != NULL;
}

bool rb9704_power_on(void)
{
	return on;
}

int rb9704_power_init(void)
{
	if (pwr.port == NULL) {
		return -ENOTSUP;
	}
	if (!gpio_is_ready_dt(&pwr) || gpio_pin_configure_dt(&pwr, GPIO_OUTPUT_INACTIVE) != 0) {
		return -EIO;
	}
	if (en.port != NULL &&
	    (!gpio_is_ready_dt(&en) || gpio_pin_configure_dt(&en, GPIO_OUTPUT_INACTIVE) != 0)) {
		return -EIO;
	}
	if (btd.port != NULL && (!gpio_is_ready_dt(&btd) || gpio_pin_configure_dt(&btd, GPIO_INPUT) != 0)) {
		return -EIO;
	}
	return 0;
}

int rb9704_power_set(bool want)
{
	if (pwr.port == NULL) {
		return -ENOTSUP;
	}
	if (!want) {
		if (en.port != NULL) {
			(void)gpio_pin_set_dt(&en, 0);
		}
		(void)gpio_pin_set_dt(&pwr, 0);
		on = false;
		return 0;
	}
	if (on) {
		return 0;
	}
	(void)gpio_pin_set_dt(&pwr, 1);
	k_msleep(10);
	if (en.port != NULL) {
		(void)gpio_pin_set_dt(&en, 1);
	}
	if (btd.port != NULL) {
		int64_t until = k_uptime_get() + BOOT_TIMEOUT_MS;

		while (gpio_pin_get_dt(&btd) != 1) {
			if (k_uptime_get() > until) {
				LOG_ERR("modem did not signal booted in %d ms", BOOT_TIMEOUT_MS);
				return -ETIMEDOUT;
			}
			k_msleep(50);
		}
	} else {
		k_msleep(DT_PROP(RB9704_NODE, boot_delay_ms));
	}
	on = true;
	return 0;
}
