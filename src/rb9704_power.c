/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rb9704_power.c - the board's control pins, as the devicetree gives them,
 * in the sequence Ground Control's hardware page sets out
 * (docs.groundcontrol.com/iot/rockblock-9704/hardware):
 *
 *   iridium-enable-gpios  I_EN, pin 3: starts and stops the modem
 *   booted-gpios          I_BTD, pin 7: high once booted, low once shut down
 *   power-gpios           P_EN, pin 6: the supercapacitor charge enable,
 *                         active low on the board, so GPIO_ACTIVE_LOW
 *
 * Up: charging on, I_EN high, then I_BTD high (or boot-delay-ms).
 * Down: I_EN low, then I_BTD low (or boot-delay-ms), then P_EN released.
 *
 * Once I_EN has moved, it must not move back until I_BTD has followed; the
 * page warns that doing so "may result in damage to the 9704 module". Both
 * directions wait for I_BTD here, and a request to go the other way waits
 * its turn on the worker thread. While I_BTD is low no input other than
 * I_EN should be driven, so P_EN is an input while the modem is off; the
 * board's pull-down on P_EN keeps the charger on then.
 *
 * P_EN was GPIO_ACTIVE_HIGH in this module's overlays until 2026-10-25,
 * which switched the charger off at every begin: the modem ran on its
 * supercapacitor and went silent 2 to 5 minutes later.
 *
 * I_EN is what switches the modem, so it alone makes the pins present;
 * P_EN and I_BTD are each optional. Called on the worker thread only.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <errno.h>

#include "rb9704_internal.h"

LOG_MODULE_DECLARE(rb9704, CONFIG_RB9704_LOG_LEVEL);

#define BTD_TIMEOUT_MS 30000

static const struct gpio_dt_spec pwr = GPIO_DT_SPEC_GET_OR(RB9704_NODE, power_gpios, {0});
static const struct gpio_dt_spec en = GPIO_DT_SPEC_GET_OR(RB9704_NODE, iridium_enable_gpios, {0});
static const struct gpio_dt_spec btd = GPIO_DT_SPEC_GET_OR(RB9704_NODE, booted_gpios, {0});

static bool on;

bool rb9704_power_present(void)
{
	return en.port != NULL;
}

bool rb9704_power_on(void)
{
	return on;
}

/* P_EN released: an input, so nothing drives the modem's pin. */
static void charge_release(void)
{
	if (pwr.port != NULL) {
		(void)gpio_pin_configure_dt(&pwr, GPIO_INPUT);
	}
}

/* Wait for I_BTD to read `level`; without I_BTD, wait boot-delay-ms. */
static int wait_booted(int level)
{
	if (btd.port == NULL) {
		k_msleep(DT_PROP(RB9704_NODE, boot_delay_ms));
		return 0;
	}
	int64_t until = k_uptime_get() + BTD_TIMEOUT_MS;

	while (gpio_pin_get_dt(&btd) != level) {
		if (k_uptime_get() > until) {
			return -ETIMEDOUT;
		}
		k_msleep(50);
	}
	return 0;
}

int rb9704_power_init(void)
{
	if (en.port == NULL) {
		return -ENOTSUP;
	}
	/* Off at boot: I_EN low. The modem may have been running from before
	 * a reset, so wait for it to finish shutting down. */
	if (!gpio_is_ready_dt(&en) || gpio_pin_configure_dt(&en, GPIO_OUTPUT_INACTIVE) != 0) {
		return -EIO;
	}
	if (btd.port != NULL && (!gpio_is_ready_dt(&btd) || gpio_pin_configure_dt(&btd, GPIO_INPUT) != 0)) {
		return -EIO;
	}
	if (pwr.port != NULL && !gpio_is_ready_dt(&pwr)) {
		return -EIO;
	}
	if (wait_booted(0) != 0) {
		LOG_WRN("modem still signals booted after I_EN went low");
	}
	charge_release();
	return 0;
}

int rb9704_power_set(bool want)
{
	if (en.port == NULL) {
		return -ENOTSUP;
	}
	if (want == on) {
		return 0;
	}
	if (!want) {
		(void)gpio_pin_set_dt(&en, 0);
		on = false;
		if (wait_booted(0) != 0) {
			LOG_ERR("modem did not signal shut down in %d ms", BTD_TIMEOUT_MS);
			return -ETIMEDOUT;
		}
		charge_release();
		return 0;
	}
	if (pwr.port != NULL) {
		(void)gpio_pin_configure_dt(&pwr, GPIO_OUTPUT_ACTIVE);	/* charging on */
	}
	(void)gpio_pin_set_dt(&en, 1);
	if (wait_booted(1) != 0) {
		LOG_ERR("modem did not signal booted in %d ms", BTD_TIMEOUT_MS);
		/* I_EN has to come down again for the next attempt to start
		 * from off, though I_BTD never followed it up. */
		(void)gpio_pin_set_dt(&en, 0);
		(void)wait_booted(0);
		charge_release();
		return -ETIMEDOUT;
	}
	on = true;
	return 0;
}
