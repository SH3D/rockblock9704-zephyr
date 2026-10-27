/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rbtest.c - `rbtest` on the shell: airtime tests with a payload that is
 * easy to check at the other end, and a record of the last MT.
 *
 *   rbtest small <topic> [count] [gap_s]   count messages of 32 bytes, one
 *                                          at a time, gap_s apart
 *   rbtest large <topic> [bytes] [confirm] one message, 50000 bytes unless
 *                                          given; without confirm it only
 *                                          says what it would cost
 *   rbtest status                          the run, and how long each took
 *   rbtest stop                            no more messages after this one
 *   rbtest mt                              the last message from the network
 *
 * A run is on a thread of its own, so the shell stays free; each message
 * is logged as it is accepted and as it finishes.
 *
 * Every test message, MO or MT, has the same shape. A 16-byte header:
 *
 *   0..2   "RBT"
 *   3      version, 1
 *   4      kind: 'S' small, 'L' large, anything else from the cloud side
 *   5      seq, from 1
 *   6      count in the run
 *   7      0
 *   8..11  the whole message's length, big-endian
 *   12..15 uptime in ms when it was made, big-endian
 *
 * then from byte 16 to the end, byte k is k % 510 counted up 0..255 and
 * back down 254..1: a ramp that shows a missing or repeated segment at a
 * glance in a hex dump. `rb9704 pattern check` in python/ checks one, and
 * `rb9704 pattern make` makes one to queue as an MT.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rb9704/rb9704.h>

#include "rbtest.h"

LOG_MODULE_REGISTER(rbtest, LOG_LEVEL_INF);

#define HEADER 16
#define SMALL_LEN 32
#define LARGE_LEN 50000
#define MAX_COUNT 20
/* How long one message may take before it is cancelled. The modem retries
 * by itself while it waits for a satellite, so these are generous. */
#define SMALL_TIMEOUT K_SECONDS(600)
#define LARGE_TIMEOUT K_SECONDS(1800)

struct fill_ctx {
	char kind;
	uint8_t seq, count;
	uint32_t uptime_ms;
};

static uint8_t ramp(size_t k)
{
	size_t v = k % 510;

	return v < 256 ? v : 510 - v;
}

static void fill(uint8_t *buf, size_t len, void *user)
{
	const struct fill_ctx *c = user;
	uint8_t h[HEADER] = { 'R', 'B', 'T', 1, c->kind, c->seq, c->count, 0 };

	sys_put_be32(len, &h[8]);
	sys_put_be32(c->uptime_ms, &h[12]);
	for (size_t k = 0; k < len; k++) {
		buf[k] = k < HEADER ? h[k] : ramp(k);
	}
}

/* -1: not a test message. Otherwise the offset of the first byte that is
 * not as expected, and len when every byte is. */
static long check(const uint8_t *d, size_t len)
{
	if (len < HEADER || memcmp(d, "RBT\x01", 4) != 0) {
		return -1;
	}
	if (sys_get_be32(&d[8]) != len) {
		return 8;
	}
	for (size_t k = HEADER; k < len; k++) {
		if (d[k] != ramp(k)) {
			return k;
		}
	}
	return len;
}

/* --- the run --------------------------------------------------------------- */

static struct {
	bool running;
	bool stop;
	char kind;
	uint16_t topic;
	uint8_t count;
	uint32_t gap_s;
	size_t len;
	uint8_t done;
	int32_t result[MAX_COUNT];	/* ms to send, or a negative errno */
	int mo_result;
} run;
static K_MUTEX_DEFINE(run_lock);
static K_SEM_DEFINE(run_go, 0, 1);
static K_SEM_DEFINE(run_mo, 0, 1);
static K_SEM_DEFINE(run_stop, 0, 1);
static volatile bool mo_waiting;

void rbtest_mo_done(uint16_t topic, uint16_t id, int result)
{
	ARG_UNUSED(topic); ARG_UNUSED(id);
	if (mo_waiting) {
		run.mo_result = result;
		k_sem_give(&run_mo);
	}
}

static int32_t one(uint8_t seq)
{
	struct fill_ctx c = { run.kind, seq, run.count, 0 };
	k_timeout_t timeout = run.kind == 'L' ? LARGE_TIMEOUT : SMALL_TIMEOUT;
	int64_t t0 = k_uptime_get();
	int rc;

	c.uptime_ms = (uint32_t)t0;
	k_sem_reset(&run_mo);
	mo_waiting = true;
	rc = rb9704_send_fill(run.topic, run.len, fill, &c, K_SECONDS(30));
	if (rc != 0 && rc != -EINPROGRESS) {
		mo_waiting = false;
		LOG_WRN("rbtest %c %u/%u: not accepted: %d", run.kind, seq, run.count, rc);
		return rc;
	}
	LOG_INF("rbtest %c %u/%u: %u bytes on topic %u, accepted in %lld ms", run.kind, seq,
		run.count, (unsigned int)run.len, run.topic, k_uptime_get() - t0);
	if (k_sem_take(&run_mo, timeout) != 0) {
		LOG_WRN("rbtest %c %u/%u: no result in %lld s, cancelling", run.kind, seq,
			run.count, (k_uptime_get() - t0) / 1000);
		(void)rb9704_cancel(K_SECONDS(5));
		(void)k_sem_take(&run_mo, K_SECONDS(60));
		mo_waiting = false;
		return -ETIMEDOUT;
	}
	mo_waiting = false;
	if (run.mo_result != 0) {
		LOG_WRN("rbtest %c %u/%u: failed after %lld ms: %d", run.kind, seq, run.count,
			k_uptime_get() - t0, run.mo_result);
		return run.mo_result;
	}
	return (int32_t)(k_uptime_get() - t0);
}

static void runner(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		uint8_t ok = 0;

		k_sem_take(&run_go, K_FOREVER);
		for (uint8_t i = 0; i < run.count && !run.stop; i++) {
			int32_t r = one(i + 1);

			k_mutex_lock(&run_lock, K_FOREVER);
			run.result[i] = r;
			run.done = i + 1;
			k_mutex_unlock(&run_lock);
			if (r >= 0) {
				ok++;
				LOG_INF("rbtest %c %u/%u: sent in %d.%03d s", run.kind, i + 1,
					run.count, r / 1000, r % 1000);
			}
			if (i + 1 < run.count && !run.stop &&
			    k_sem_take(&run_stop, K_SECONDS(run.gap_s)) == 0) {
				break;
			}
		}
		LOG_INF("rbtest %c: %u of %u sent%s", run.kind, ok, run.count,
			run.done < run.count ? ", stopped" : "");
		k_mutex_lock(&run_lock, K_FOREVER);
		run.running = false;
		k_mutex_unlock(&run_lock);
	}
}

K_THREAD_DEFINE(rbtest_tid, 2048, runner, NULL, NULL, NULL, 10, 0, 0);

static int start(const struct shell *sh, char kind, uint16_t topic, uint8_t count,
		 uint32_t gap_s, size_t len)
{
	k_mutex_lock(&run_lock, K_FOREVER);
	if (run.running) {
		k_mutex_unlock(&run_lock);
		shell_error(sh, "a run is going: `rbtest status`, `rbtest stop`");
		return -EBUSY;
	}
	run.running = true;
	run.stop = false;
	run.kind = kind;
	run.topic = topic;
	run.count = count;
	run.gap_s = gap_s;
	run.len = len;
	run.done = 0;
	k_mutex_unlock(&run_lock);
	k_sem_reset(&run_stop);
	k_sem_give(&run_go);
	return 0;
}

static int parse_topic(const struct shell *sh, const char *s, uint16_t *topic)
{
	unsigned long t = strtoul(s, NULL, 10);

	if (t < RB9704_TOPIC_MIN || t > UINT16_MAX) {
		shell_error(sh, "topic %s: from %u, 244 is raw", s, RB9704_TOPIC_MIN);
		return -EINVAL;
	}
	*topic = (uint16_t)t;
	return 0;
}

static int cmd_small(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t topic;
	unsigned long count = argc > 2 ? strtoul(argv[2], NULL, 10) : 3;
	unsigned long gap = argc > 3 ? strtoul(argv[3], NULL, 10) : 60;

	if (parse_topic(sh, argv[1], &topic) != 0) {
		return -EINVAL;
	}
	if (count < 1 || count > MAX_COUNT) {
		shell_error(sh, "count 1 to %d", MAX_COUNT);
		return -EINVAL;
	}
	shell_print(sh, "%lu messages of %d bytes on topic %u, %lu s apart: this costs airtime",
		    count, SMALL_LEN, topic, gap);
	return start(sh, 'S', topic, count, gap, SMALL_LEN);
}

static int cmd_large(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t topic;
	unsigned long len = LARGE_LEN;
	bool confirmed = false;

	if (parse_topic(sh, argv[1], &topic) != 0) {
		return -EINVAL;
	}
	for (size_t i = 2; i < argc; i++) {
		if (strcmp(argv[i], "confirm") == 0) {
			confirmed = true;
		} else {
			len = strtoul(argv[i], NULL, 10);
		}
	}
	if (len <= HEADER || len > CONFIG_RB9704_MESSAGE_MAX) {
		shell_error(sh, "%lu bytes: from %d to %d, CONFIG_RB9704_MESSAGE_MAX in this build",
			    len, HEADER + 1, CONFIG_RB9704_MESSAGE_MAX);
		return -EINVAL;
	}
	if (!confirmed) {
		shell_print(sh, "one message of %lu bytes on topic %u, %lu times the size of a",
			    len, topic, len / SMALL_LEN);
		shell_print(sh, "small test message. It costs airtime for every byte.");
		shell_print(sh, "Nothing sent. To send it: rbtest large %u %lu confirm", topic, len);
		return 0;
	}
	shell_print(sh, "%lu bytes on topic %u: this costs airtime", len, topic);
	return start(sh, 'L', topic, 1, 0, len);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	k_mutex_lock(&run_lock, K_FOREVER);
	if (run.count == 0) {
		shell_print(sh, "no run since boot");
	} else {
		shell_print(sh, "%s %c: %u of %u done, %u bytes each on topic %u",
			    run.running ? "running" : "last run", run.kind, run.done, run.count,
			    (unsigned int)run.len, run.topic);
		for (int i = 0; i < run.done; i++) {
			int32_t r = run.result[i];

			if (r >= 0) {
				shell_print(sh, "  %d: sent in %d.%03d s", i + 1, r / 1000,
					    r % 1000);
			} else {
				shell_print(sh, "  %d: failed, %d", i + 1, r);
			}
		}
	}
	k_mutex_unlock(&run_lock);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	if (!run.running) {
		shell_print(sh, "nothing running");
		return 0;
	}
	run.stop = true;
	k_sem_give(&run_stop);
	shell_print(sh, "stopping after the message in flight; `rb9704 cancel` abandons it");
	return 0;
}

/* --- the last MT ------------------------------------------------------------ */

static struct {
	uint32_t count;
	int64_t at_ms;
	uint16_t topic, id;
	size_t len;
	uint32_t crc;
	uint8_t head[32];
	long check;
} mt_last;
static K_MUTEX_DEFINE(mt_lock);

void rbtest_mt(const struct rb9704_mt *mt)
{
	long c = check(mt->data, mt->len);

	k_mutex_lock(&mt_lock, K_FOREVER);
	mt_last.count++;
	mt_last.at_ms = k_uptime_get();
	mt_last.topic = mt->topic;
	mt_last.id = mt->id;
	mt_last.len = mt->len;
	mt_last.crc = crc32_ieee(mt->data, mt->len);
	memcpy(mt_last.head, mt->data, MIN(mt->len, sizeof(mt_last.head)));
	mt_last.check = c;
	k_mutex_unlock(&mt_lock);
	if (c == (long)mt->len) {
		LOG_INF("rbtest: MT is a test message, %c %u/%u, all %u bytes as expected",
			mt->data[4], mt->data[5], mt->data[6], (unsigned int)mt->len);
	} else if (c >= 0) {
		LOG_WRN("rbtest: MT is a test message, wrong from byte %ld of %u", c,
			(unsigned int)mt->len);
	}
}

static int cmd_mt(const struct shell *sh, size_t argc, char **argv)
{
	char hex[3 * sizeof(mt_last.head) + 1];
	char text[sizeof(mt_last.head) + 1];
	size_t n;

	ARG_UNUSED(argc); ARG_UNUSED(argv);
	k_mutex_lock(&mt_lock, K_FOREVER);
	if (mt_last.count == 0) {
		k_mutex_unlock(&mt_lock);
		shell_print(sh, "no MT since boot");
		return 0;
	}
	n = MIN(mt_last.len, sizeof(mt_last.head));
	for (size_t i = 0; i < n; i++) {
		snprintk(&hex[3 * i], 4, "%02x ", mt_last.head[i]);
		text[i] = isprint(mt_last.head[i]) ? mt_last.head[i] : '.';
	}
	hex[n > 0 ? 3 * n - 1 : 0] = '\0';
	text[n] = '\0';
	shell_print(sh, "mt        %u since boot, the last %lld s ago", mt_last.count,
		    (k_uptime_get() - mt_last.at_ms) / 1000);
	shell_print(sh, "          topic %u id %u, %u bytes, crc %08x", mt_last.topic, mt_last.id,
		    (unsigned int)mt_last.len, mt_last.crc);
	shell_print(sh, "          %s", hex);
	shell_print(sh, "          %s", text);
	if (mt_last.check == (long)mt_last.len) {
		shell_print(sh, "          test message %c %u/%u, all %u bytes as expected",
			    mt_last.head[4], mt_last.head[5], mt_last.head[6],
			    (unsigned int)mt_last.len);
	} else if (mt_last.check >= 0) {
		shell_print(sh, "          test message, wrong from byte %ld", mt_last.check);
	}
	k_mutex_unlock(&mt_lock);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(rbtest_cmds,
	SHELL_CMD_ARG(small, NULL, "small <topic> [count=3] [gap_s=60]: costs airtime",
		      cmd_small, 2, 2),
	SHELL_CMD_ARG(large, NULL, "large <topic> [bytes=50000] [confirm]: costs airtime",
		      cmd_large, 2, 2),
	SHELL_CMD(status, NULL, "the run, and how long each message took", cmd_status),
	SHELL_CMD(stop, NULL, "no more messages after the one in flight", cmd_stop),
	SHELL_CMD(mt, NULL, "the last message from the network", cmd_mt),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(rbtest, &rbtest_cmds, "airtime tests with a checkable payload", NULL);
