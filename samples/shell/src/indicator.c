/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * indicator.c - the board's RGB LED and button, for a board out in the open
 * with nobody at its console. Built on boards whose devicetree has a
 * led-strip alias (one LED is enough) and an sw0 button.
 *
 * The LED, by priority:
 *
 *   white, fast blink   a button action is running (begin, end)
 *   green flash         a message went (1 s)
 *   red flash           a message, or a button action, failed (2 s)
 *   cyan flash          a message arrived (2 s)
 *   magenta             a message is with the modem, on its way
 *   red/yellow/green    begun: no signal or no satellite / 1-2 bars / 3-5
 *   blue                not begun
 *
 * On a WiFi build the steady colours blink slowly while there is no WiFi:
 * blinking means telnet will not reach it.
 *
 * The button, both free of airtime:
 *
 *   short press   not begun: rb9704_begin. Begun: ask for the signal.
 *   long press    rb9704_end, and the supply off when there is a power pin.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <rb9704/rb9704.h>

#include "indicator.h"
#ifdef CONFIG_WIFI
#include "wifi.h"
#endif

LOG_MODULE_REGISTER(sample_indicator, LOG_LEVEL_INF);

#define TICK_MS	   100
#define LONG_MS	   2000
/* Out of 255: the SK6812 at full is too bright to look at. */
#define LEVEL	   24

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(led_strip));

enum flash { FLASH_NONE, FLASH_SENT, FLASH_FAILED, FLASH_MT };

static atomic_t flash;
static int64_t flash_until;
static atomic_t busy;
static uint32_t ticks;

static K_SEM_DEFINE(press_sem, 0, 1);
static bool press_long;

static struct led_rgb rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return (struct led_rgb){.r = r, .g = g, .b = b};
}

static const struct led_rgb off = {0};

static bool wifi_up(void)
{
#ifdef CONFIG_WIFI
	return sample_wifi_connected();
#else
	return true;
#endif
}

void indicator_flash_mo(int result)
{
	atomic_set(&flash, result == 0 ? FLASH_SENT : FLASH_FAILED);
	flash_until = k_uptime_get() + (result == 0 ? 1000 : 2000);
}

void indicator_flash_mt(void)
{
	atomic_set(&flash, FLASH_MT);
	flash_until = k_uptime_get() + 2000;
}

static struct led_rgb pick(void)
{
	struct rb9704_status st;
	struct led_rgb c;

	ticks++;
	if (atomic_get(&busy)) {
		return (ticks & 1) ? rgb(LEVEL, LEVEL, LEVEL) : off;
	}
	if (atomic_get(&flash) != FLASH_NONE) {
		if (k_uptime_get() < flash_until) {
			switch (atomic_get(&flash)) {
			case FLASH_SENT:
				return rgb(0, LEVEL, 0);
			case FLASH_FAILED:
				return rgb(LEVEL, 0, 0);
			default:
				return rgb(0, LEVEL, LEVEL);
			}
		}
		atomic_set(&flash, FLASH_NONE);
	}

	rb9704_status(&st);
	if (st.mo_in_flight) {
		c = rgb(LEVEL, 0, LEVEL);
	} else if (!st.begun) {
		c = rgb(0, 0, LEVEL);
	} else if (!st.signal_known || st.signal_bars == 0 ||
		   (st.visibility_known && !st.constellation_visible)) {
		c = rgb(LEVEL, 0, 0);
	} else if (st.signal_bars < 3) {
		c = rgb(LEVEL, LEVEL, 0);
	} else {
		c = rgb(0, LEVEL, 0);
	}
	/* Half a second on, half off, while telnet cannot reach the board. */
	if (!wifi_up() && (ticks / 5) % 2) {
		return off;
	}
	return c;
}

static void tick(struct k_work *work)
{
	struct led_rgb c = pick();

	(void)led_strip_update_rgb(strip, &c, 1);
	k_work_schedule(k_work_delayable_from_work(work), K_MSEC(TICK_MS));
}

static K_WORK_DELAYABLE_DEFINE(tick_work, tick);

/* The input thread must not wait on the modem: a press is handed to the
 * button thread, and a press during an action is dropped. */
static void on_key(struct input_event *evt, void *user)
{
	static int64_t down;

	ARG_UNUSED(user);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->value) {
		down = k_uptime_get();
		return;
	}
	if (atomic_get(&busy)) {
		return;
	}
	press_long = k_uptime_get() - down >= LONG_MS;
	k_sem_give(&press_sem);
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_PARENT(DT_ALIAS(sw0))), on_key, NULL);

static void button_thread(void *a, void *b, void *c)
{
	struct rb9704_status st;
	int rc;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&press_sem, K_FOREVER);
		rb9704_status(&st);
		atomic_set(&busy, 1);
		if (press_long) {
			LOG_INF("button: end");
			/* rb9704_power ends the library first. */
			rc = rb9704_power(false);
			if (rc == -ENOTSUP) {
				rc = st.begun ? rb9704_end(K_SECONDS(10)) : 0;
			}
		} else if (!st.begun) {
			LOG_INF("button: begin");
			rc = rb9704_begin(K_SECONDS(60));
		} else {
			LOG_INF("button: signal");
			rc = rb9704_signal(K_SECONDS(10));
		}
		atomic_set(&busy, 0);
		if (rc < 0) {
			LOG_WRN("button: failed (%d)", rc);
			indicator_flash_mo(rc);
		}
	}
}

K_THREAD_DEFINE(button_tid, 2048, button_thread, NULL, NULL, NULL, 10, 0, 0);

void indicator_start(void)
{
	if (!device_is_ready(strip)) {
		LOG_ERR("LED not ready");
		return;
	}
	k_work_schedule(&tick_work, K_NO_WAIT);
}
