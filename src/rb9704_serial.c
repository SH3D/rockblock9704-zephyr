/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rb9704_serial.c - the library's serial port on a Zephyr UART. This is the
 * whole port: five function pointers filled into the library's global
 * serialContext by rb9704_serial_set_context(), which rbBegin() reaches
 * through the SERIAL_CONTEXT_SETUP_FUNC macro. The UART is the bus of the
 * "rock7,rockblock-9704" devicetree node.
 *
 * RX rides the UART ISR into a ring buffer sized for the library's largest
 * JSPR line (8 kB, matching its jspr RX buffer); the library then pulls
 * bytes one at a time from receiveJspr(). TX is polled: a worst-case 2 kB
 * command is about 90 ms at 230400, and only the worker thread ever writes.
 *
 * The receive interrupt is on from boot, not only while the library is
 * open. A modem powered up by `rb9704 power on`, or left talking after
 * rbEnd(), sends lines nobody is reading; with the interrupt off they
 * overflowed the UART's hardware FIFO, 128 bytes on the ESP32-C3, and the
 * UART then gave back repeated fragments of old lines and never the
 * replies (2026-10-25, M5Stamp-C3). Now the ISR always empties the FIFO
 * and throws the bytes away while the library is closed. When it opens
 * part-way through a line, the rest of that line is thrown away too: a
 * line with no status code at its front makes the library read past its
 * buffer (docs/JSPR.md, section 8, item 2), and that crashed the board.
 *
 * The spy (rb9704_spy): JSPR is printable JSON lines, so the spy reassembles
 * the lines in each direction and printk()s them. Do not use the library's
 * own -DDEBUG for this: its TX debug path truncates the caller's buffer in
 * place.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <errno.h>

#include "rb9704_glue.h"
#include "rb9704_internal.h"
#include "serial.h"		/* the library's serialContext */

/* Defined in the library's serial.c; each preset declares it itself. */
extern serialContext context;

static const struct device *const modem_uart = DEVICE_DT_GET(DT_BUS(RB9704_NODE));

RING_BUF_DECLARE(rb9704_rx_ring, CONFIG_RB9704_RX_RING_SIZE);
static K_SEM_DEFINE(rb9704_rx_sem, 0, 1);
static atomic_t rx_dropped;
static atomic_t spy_on;

/* Whether the library is open, so received bytes are for it. */
static atomic_t listening;
/* ISR only: the last byte heard ended a line, or there was none yet.
 * Non-printable bytes do not count: the modem sends a few at boot, and the
 * library already skips junk in front of a status code. */
static bool mid_line;
/* Set as the library opens mid-line: drop bytes up to the next CR. */
static atomic_t skip_to_cr;

/* Per-byte RX timeout. receiveJspr() treats a 0-return mid-line as
 * end-of-data and discards the partial line, so this must comfortably
 * exceed any inter-byte gap the modem produces (gaps at 230400 are
 * microseconds; 500 ms is one bounded worker stall, worst case). */
#define RX_BYTE_TIMEOUT_MS 500

/* --- the spy ------------------------------------------------------ */

struct spy_line {
	char buf[192];
	size_t len;
};

static struct spy_line spy_rx = { .len = 0 };
static struct spy_line spy_tx = { .len = 0 };

static void spy_flush(struct spy_line *l, const char *tag)
{
	if (l->len == 0) {
		return;
	}
	l->buf[l->len] = '\0';
	printk("rb9704 %s: %s\n", tag, l->buf);
	l->len = 0;
}

static void spy_feed(struct spy_line *l, const char *tag,
		     const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		char c = data[i];

		if (c == '\n' || c == '\r') {
			spy_flush(l, tag);
			continue;
		}
		l->buf[l->len++] = (c >= 0x20 && c < 0x7f) ? c : '.';
		if (l->len >= sizeof(l->buf) - 1) {
			spy_flush(l, tag);
		}
	}
}

void rb9704_serial_spy(bool on)
{
	atomic_set(&spy_on, on ? 1 : 0);
	if (!on) {
		spy_rx.len = 0;
		spy_tx.len = 0;
	}
}

/* --- ISR ---------------------------------------------------------------- */

static void modem_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t tmp[64];
		int n = uart_fifo_read(dev, tmp, sizeof(tmp));
		int keep = 0;

		if (n <= 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			uint8_t c = tmp[i];
			bool skip = !atomic_get(&listening) || atomic_get(&skip_to_cr);

			if (c == '\r') {
				mid_line = false;
				if (atomic_get(&skip_to_cr)) {
					atomic_clear(&skip_to_cr);
				}
			} else if (c >= 0x20 && c < 0x7f) {
				mid_line = true;
			}
			if (!skip) {
				tmp[keep++] = c;
			}
		}
		if (keep == 0) {
			continue;
		}
		uint32_t put = ring_buf_put(&rb9704_rx_ring, tmp, keep);

		if (put < (uint32_t)keep) {
			atomic_add(&rx_dropped, keep - put);
		}
		k_sem_give(&rb9704_rx_sem);
		rb9704_worker_rx();
	}
}

/* --- the five serialContext functions ----------------------------------- */

static bool ser_init(void)
{
	if (!device_is_ready(modem_uart)) {
		printk("rb9704: UART %s not ready\n", modem_uart->name);
		return false;
	}
	/* The ISR does not touch the ring while not listening, so the reset
	 * needs no lock. */
	ring_buf_reset(&rb9704_rx_ring);
	k_sem_reset(&rb9704_rx_sem);
	atomic_set(&skip_to_cr, mid_line ? 1 : 0);
	atomic_set(&listening, 1);
	return true;
}

static bool ser_deinit(void)
{
	/* The interrupt stays on, emptying the FIFO into nothing. */
	atomic_clear(&listening);
	return true;
}

static int ser_read(char *bytes, const uint16_t length)
{
	int64_t deadline = k_uptime_get() + RX_BYTE_TIMEOUT_MS;

	for (;;) {
		int n = ring_buf_get(&rb9704_rx_ring, (uint8_t *)bytes, length);

		if (n > 0) {
			if (atomic_get(&spy_on)) {
				spy_feed(&spy_rx, "RX", bytes, n);
			}
			return n;
		}

		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			return 0;
		}
		k_sem_take(&rb9704_rx_sem, K_MSEC(left));
	}
}

static int ser_write(const char *data, const uint16_t length)
{
	if (atomic_get(&spy_on)) {
		spy_feed(&spy_tx, "TX", data, length);
	}
	for (uint16_t i = 0; i < length; i++) {
		uart_poll_out(modem_uart, data[i]);
	}
	return length;
}

static int ser_peek(void)
{
	return (int)ring_buf_size_get(&rb9704_rx_ring);
}

/* --- setup + worker-facing helpers -------------------------------------- */

int rb9704_serial_start(void)
{
	if (!device_is_ready(modem_uart)) {
		return -ENODEV;
	}
	uart_irq_callback_user_data_set(modem_uart, modem_uart_isr, NULL);
	uart_irq_rx_enable(modem_uart);
	return 0;
}

bool rb9704_serial_set_context(const char *port, uint32_t baud)
{
	ARG_UNUSED(port);	/* the UART is the devicetree node's bus */

	context.serialInit = ser_init;
	context.serialDeInit = ser_deinit;
	context.serialRead = ser_read;
	context.serialWrite = ser_write;
	context.serialPeek = ser_peek;
	context.serialBaud = baud;	/* informational; rate is set by DT */
	return true;
}

int rb9704_serial_available(void)
{
	return (int)ring_buf_size_get(&rb9704_rx_ring);
}

uint32_t rb9704_serial_dropped(void)
{
	return (uint32_t)atomic_get(&rx_dropped);
}
