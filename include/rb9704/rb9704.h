/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rb9704.h - the RockBLOCK 9704 Iridium modem on Zephyr.
 *
 * Ground Control's C library (github.com/rock7/RockBLOCK-9704) does the
 * protocol: JSPR, JSON lines over the modem's UART, and the segmenting of
 * messages to and from the Iridium network. It has no locking, shares
 * static buffers across its calls, and its blocking calls stop working once
 * callbacks are registered. So this module gives the library one thread of
 * its own, the worker, and every call into it happens there. What this
 * header offers is that thread's front door:
 *
 *   - requests (begin, end, send, info, signal) handed to the worker, each
 *     waiting as long as the caller says and no longer: K_NO_WAIT queues
 *     the request and returns, which is what a thread that must never
 *     block wants;
 *   - callbacks from the worker when a message has gone, a message has
 *     arrived, the signal changes or the provisioning is known;
 *   - a status snapshot that never touches the modem.
 *
 * One modem per system: the library keeps global state. The modem is the
 * first devicetree node with compatible "rock7,rockblock-9704", a child of
 * the UART it sits on (dts/bindings/rock7,rockblock-9704.yaml).
 *
 * Every call returns 0 or a negative errno, and never costs airtime except
 * rb9704_send.
 */
#ifndef RB9704_RB9704_H_
#define RB9704_RB9704_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Topics below this are the network's own; the library refuses them. */
#define RB9704_TOPIC_MIN 64
/* Cloudloop's default topic for raw messages (cloudloopTopics_t). */
#define RB9704_TOPIC_RAW 244

#define RB9704_MAX_TOPICS 8

struct rb9704_topic {
	uint16_t id;
	char name[57];
};

/* What the modem says about itself; rb9704_info() fills it. */
struct rb9704_info {
	char imei[16];
	char serial[16];
	char hw_version[16];
	char fw_version[16];
	char iccid[23];
	int8_t board_temp_c;
	bool card_present;
	bool sim_connected;
};

/* A snapshot for status displays; never talks to the modem. */
struct rb9704_status {
	bool powered;		/* the supply is on, or there is no power pin */
	bool begun;		/* rb9704_begin has run and the library is up */
	/* signal, from the modem's unsolicited constellation state */
	bool signal_known;
	bool visibility_known;	/* a constellation report has said */
	bool constellation_visible;
	uint8_t signal_bars;	/* 0 to 5 */
	int16_t signal_level;
	int64_t signal_ms;	/* uptime of the last report */
	/* provisioning: the topics this modem may send on */
	bool provisioning_known;
	uint8_t topic_count;
	struct rb9704_topic topics[RB9704_MAX_TOPICS];
	/* messages */
	bool mo_in_flight;
	uint16_t mo_topic;
	uint32_t mo_sent, mo_failed;
	uint32_t mt_received, mt_failed;
	/* the serial port */
	uint32_t rx_dropped;	/* bytes the receive ring had no room for */
};

/* A message from the network, valid for the length of the callback. */
struct rb9704_mt {
	uint16_t topic;
	uint16_t id;
	const uint8_t *data;
	size_t len;
};

/* Called on the worker thread. Keep them short and never call back into a
 * blocking rb9704_* request from one: the worker is the thread that would
 * have to answer it. Any member may be NULL. */
struct rb9704_callbacks {
	/* An MO has finished: result 0 when the network has it. */
	void (*mo_done)(uint16_t topic, uint16_t id, int result, void *user);
	/* An MT has arrived whole. It is acknowledged to the modem after this
	 * returns, so copy what is wanted. */
	void (*mt)(const struct rb9704_mt *mt, void *user);
	/* The constellation state changed. */
	void (*signal)(uint8_t bars, bool visible, void *user);
	/* The modem reported its provisioning. */
	void (*provisioning)(const struct rb9704_topic *topics, uint8_t count, void *user);
	void *user;
};

/* Replace the application's callbacks; NULL clears them. */
void rb9704_set_callbacks(const struct rb9704_callbacks *cb);

/* Switch the board's supply and modem enable, when the devicetree gives the
 * pins. Waits for the booted pin or boot-delay-ms on the way up. -ENOTSUP
 * with no power pin. rb9704_end first if it was begun. */
int rb9704_power(bool on);

/* Open the serial port and bring the library up: API version, SIM
 * interface, operational state. Powers the modem first if there is a pin
 * and it is off. -EALREADY when begun. */
int rb9704_begin(k_timeout_t timeout);

/* Close it down. -EALREADY when not begun. */
int rb9704_end(k_timeout_t timeout);

/* Hand a message to the modem for the network. The data is copied, so the
 * caller's buffer is free on return. Returns 0 once the modem has accepted
 * it; completion comes through mo_done. With K_NO_WAIT it queues and
 * returns -EINPROGRESS, and a refusal also comes through mo_done.
 *
 *   -EINVAL    topic below RB9704_TOPIC_MIN, or len 0 or over
 *              CONFIG_RB9704_MESSAGE_MAX
 *   -ENOTCONN  not begun
 *   -EBUSY     a message is already in flight
 *   -EACCES    the modem is not provisioned for this topic
 *   -EAGAIN    the request slot was taken and the timeout ran out
 */
int rb9704_send(uint16_t topic, const void *data, size_t len, k_timeout_t timeout);

/* Ask the modem who it is. Free, milliseconds. */
int rb9704_info(struct rb9704_info *out, k_timeout_t timeout);

/* Ask the modem for its signal now. Returns 0 to 5 bars, or -errno. */
int rb9704_signal(k_timeout_t timeout);

void rb9704_status(struct rb9704_status *out);

/* Print every JSPR line in both directions with printk, for debugging. */
void rb9704_spy(bool on);

#ifdef __cplusplus
}
#endif

#endif /* RB9704_RB9704_H_ */
