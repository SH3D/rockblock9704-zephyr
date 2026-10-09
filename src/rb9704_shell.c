/* SPDX-License-Identifier: MIT
 *
 * rb9704_shell.c - `rb9704` on the Zephyr shell.
 *
 * Everything here is local serial traffic and free, except `send`, which
 * costs airtime. Messages from the network arrive through the
 * application's mt callback; the sample prints them.
 */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rb9704/rb9704.h>

#define WAIT K_SECONDS(30)

/* The errno values differ between C libraries, so name the ones this
 * module returns. */
static const char *err_name(int rc)
{
	switch (-rc) {
	case EALREADY: return "EALREADY";
	case ENOTCONN: return "ENOTCONN";
	case EBUSY: return "EBUSY";
	case EACCES: return "EACCES";
	case EAGAIN: return "EAGAIN";
	case EINVAL: return "EINVAL";
	case EIO: return "EIO";
	case ETIMEDOUT: return "ETIMEDOUT";
	case EINPROGRESS: return "EINPROGRESS";
	case ENOTSUP: return "ENOTSUP";
	case EDEADLK: return "EDEADLK";
	default: return "?";
	}
}

static int report(const struct shell *sh, const char *what, int rc)
{
	if (rc < 0) {
		shell_error(sh, "%s: %d %s", what, rc, err_name(rc));
		return rc;
	}
	shell_print(sh, "%s: ok", what);
	return 0;
}

static int cmd_begin(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return report(sh, "begin", rb9704_begin(WAIT));
}

static int cmd_end(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	return report(sh, "end", rb9704_end(WAIT));
}

static int cmd_power(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return report(sh, "power", rb9704_power(strcmp(argv[1], "on") == 0));
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct rb9704_status s;

	ARG_UNUSED(argc); ARG_UNUSED(argv);
	rb9704_status(&s);
	shell_print(sh, "powered   %s, begun %s", s.powered ? "yes" : "no", s.begun ? "yes" : "no");
	if (s.signal_known) {
		shell_print(sh, "signal    %u bars%s, %lld s ago", s.signal_bars,
			    !s.visibility_known ? ""
			    : s.constellation_visible ? ", constellation visible"
						      : ", constellation not visible",
			    (k_uptime_get() - s.signal_ms) / 1000);
	} else {
		shell_print(sh, "signal    unknown");
	}
	if (s.provisioning_known) {
		shell_print(sh, "topics    %u", s.topic_count);
		for (int i = 0; i < s.topic_count; i++) {
			shell_print(sh, "          %u %s", s.topics[i].id, s.topics[i].name);
		}
	} else {
		shell_print(sh, "topics    provisioning not seen yet");
	}
	shell_print(sh, "mo        %u sent, %u failed%s", s.mo_sent, s.mo_failed,
		    s.mo_in_flight ? ", one in flight" : "");
	shell_print(sh, "mt        %u received, %u failed", s.mt_received, s.mt_failed);
	shell_print(sh, "serial    %u bytes dropped", s.rx_dropped);
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	struct rb9704_info i;
	int rc;

	ARG_UNUSED(argc); ARG_UNUSED(argv);
	rc = rb9704_info(&i, WAIT);
	if (rc != 0) {
		return report(sh, "info", rc);
	}
	shell_print(sh, "imei      %s", i.imei);
	shell_print(sh, "serial    %s", i.serial);
	shell_print(sh, "hardware  %s", i.hw_version);
	shell_print(sh, "firmware  %s", i.fw_version);
	shell_print(sh, "iccid     %s", i.iccid);
	shell_print(sh, "board     %d C", i.board_temp_c);
	shell_print(sh, "sim       card %s, %s", i.card_present ? "present" : "absent",
		    i.sim_connected ? "connected" : "not connected");
	return 0;
}

static int cmd_signal(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	ARG_UNUSED(argc); ARG_UNUSED(argv);
	rc = rb9704_signal(WAIT);
	if (rc < 0) {
		return report(sh, "signal", rc);
	}
	shell_print(sh, "signal: %d bars", rc);
	return 0;
}

static int unhex(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;

	while (s[0] != '\0' && s[1] != '\0') {
		char b[3] = { s[0], s[1], '\0' };
		char *end;

		if (n == cap) {
			return -ENOSPC;
		}
		out[n++] = (uint8_t)strtoul(b, &end, 16);
		if (*end != '\0') {
			return -EINVAL;
		}
		s += 2;
	}
	return s[0] == '\0' ? (int)n : -EINVAL;
}

static int cmd_send(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t buf[CONFIG_RB9704_MESSAGE_MAX];
	unsigned long topic = strtoul(argv[1], NULL, 10);
	bool hex = strcmp(argv[0], "sendhex") == 0;
	int len;

	ARG_UNUSED(argc);
	if (hex) {
		len = unhex(argv[2], buf, sizeof(buf));
	} else {
		len = MIN(strlen(argv[2]), sizeof(buf));
		memcpy(buf, argv[2], len);
	}
	if (len <= 0) {
		shell_error(sh, "nothing to send (%d)", len);
		return -EINVAL;
	}
	shell_print(sh, "%d bytes on topic %lu: this costs airtime", len, topic);
	return report(sh, "send", rb9704_send((uint16_t)topic, buf, len, WAIT));
}

static int cmd_spy(const struct shell *sh, size_t argc, char **argv)
{
	bool on = strcmp(argv[1], "on") == 0;

	ARG_UNUSED(argc);
	rb9704_spy(on);
	shell_print(sh, "spy %s", on ? "on: every JSPR line printed" : "off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(rb9704_cmds,
	SHELL_CMD(begin, NULL, "open the port and bring the library up", cmd_begin),
	SHELL_CMD(end, NULL, "close it down", cmd_end),
	SHELL_CMD_ARG(power, NULL, "power on|off: the board's supply, if wired", cmd_power, 2, 0),
	SHELL_CMD(status, NULL, "what is known, without asking the modem", cmd_status),
	SHELL_CMD(info, NULL, "IMEI, serial, versions, ICCID, SIM, temperature", cmd_info),
	SHELL_CMD(signal, NULL, "signal bars now", cmd_signal),
	SHELL_CMD_ARG(send, NULL, "send <topic> <text>: one message, costs airtime", cmd_send, 3, 0),
	SHELL_CMD_ARG(sendhex, NULL, "sendhex <topic> <hex>: one message, costs airtime", cmd_send, 3, 0),
	SHELL_CMD_ARG(spy, NULL, "spy on|off: print every JSPR line", cmd_spy, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(rb9704, &rb9704_cmds, "RockBLOCK 9704 Iridium modem", NULL);
