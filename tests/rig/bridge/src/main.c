/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * A two-way UART bridge, for the test rig: every byte in on `host` goes out
 * on `target`, and the other way round. With `host` a board's USB serial
 * port and `target` a UART wired to another board's modem pins, the fake
 * modem on the host answers that board as a modem would. The aliases are
 * the board overlay's. */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

static const struct device *const host = DEVICE_DT_GET(DT_ALIAS(bridge_host));
static const struct device *const target = DEVICE_DT_GET(DT_ALIAS(bridge_target));

RING_BUF_DECLARE(to_target, 4096);
RING_BUF_DECLARE(to_host, 4096);

static void isr(const struct device *dev, void *user)
{
	struct ring_buf *out = user;	/* bytes read here go into this ring */
	const struct device *other = dev == host ? target : host;
	struct ring_buf *back = out == &to_target ? &to_host : &to_target;
	uint8_t b[64];

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			int n = uart_fifo_read(dev, b, sizeof(b));

			if (n > 0) {
				ring_buf_put(out, b, n);
				uart_irq_tx_enable(other);
			}
		}
		if (uart_irq_tx_ready(dev)) {
			uint8_t *p;
			uint32_t n = ring_buf_get_claim(back, &p, 64);

			if (n == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int w = uart_fifo_fill(dev, p, n);

				ring_buf_get_finish(back, w > 0 ? w : 0);
			}
		}
	}
}

int main(void)
{
	uart_irq_callback_user_data_set(host, isr, &to_target);
	uart_irq_callback_user_data_set(target, isr, &to_host);
	uart_irq_rx_enable(host);
	uart_irq_rx_enable(target);
	return 0;
}
