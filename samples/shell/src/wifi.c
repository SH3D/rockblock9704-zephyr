/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * wifi.c - join the stored network at boot, and keep trying, so a board out
 * in the open is reachable by telnet with nobody at its USB port. Built with
 * overlay-wifi.conf only.
 *
 * The credentials are the ones `wifi cred add` kept in flash. The ESP32
 * driver reconnects by itself after a drop (CONFIG_ESP32_WIFI_STA_RECONNECT);
 * the retry here covers a network that was not there at boot.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_credentials.h>

#include "wifi.h"

LOG_MODULE_REGISTER(sample_wifi, LOG_LEVEL_INF);

#define RETRY K_SECONDS(30)

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ip_cb;
static struct k_work_delayable retry;
static bool connected;

static void try_connect(struct k_work *work)
{
	static bool warned;
	struct net_if *iface = net_if_get_wifi_sta();

	ARG_UNUSED(work);
	if (connected || iface == NULL) {
		return;
	}
	if (wifi_credentials_is_empty()) {
		/* Checked again in 30 s, so a network added now is joined
		 * without a reboot. */
		if (!warned) {
			LOG_WRN("no network stored: wifi cred add -s \"<SSID>\" -k 1 -p \"<password>\"");
			warned = true;
		}
		k_work_reschedule(&retry, RETRY);
		return;
	}
	if (net_mgmt(NET_REQUEST_WIFI_CONNECT_STORED, iface, NULL, 0) != 0) {
		k_work_reschedule(&retry, RETRY);
	}
}

static void on_wifi(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = cb->info;

	ARG_UNUSED(iface);
	if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
		connected = status->status == 0;
		if (connected) {
			LOG_INF("wifi connected, waiting for an address");
		} else {
			LOG_WRN("wifi connect failed (%d), again in 30 s", status->status);
			k_work_reschedule(&retry, RETRY);
		}
	} else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		connected = false;
		LOG_WRN("wifi disconnected");
		k_work_reschedule(&retry, RETRY);
	}
}

static void on_ip(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	char addr[NET_IPV4_ADDR_LEN];

	ARG_UNUSED(cb);
	ARG_UNUSED(event);
	ARRAY_FOR_EACH(iface->config.ip.ipv4->unicast, i) {
		const struct net_if_addr *a = &iface->config.ip.ipv4->unicast[i].ipv4;

		if (a->is_used && a->addr_type == NET_ADDR_DHCP) {
			net_addr_ntop(AF_INET, &a->address.in_addr, addr, sizeof(addr));
			LOG_INF("address %s: telnet %s", addr, addr);
		}
	}
}

bool sample_wifi_connected(void)
{
	return connected;
}

void sample_wifi_start(void)
{
	k_work_init_delayable(&retry, try_connect);
	net_mgmt_init_event_callback(&wifi_cb, on_wifi,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ip_cb, on_ip, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ip_cb);
	/* A moment for the driver to bring the interface up. */
	k_work_reschedule(&retry, K_SECONDS(1));
}
