/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Scott Penrose <scottp@dd.com.au>
 *
 * rb9704_worker.c - the one thread that calls into Ground Control's library.
 *
 * The library has no locking and shares static buffers between its calls,
 * so every rb* call happens here. Its callbacks fire inside rbPoll(), on
 * this thread, and only record; the loop does the work after.
 *
 *   loop     sleep until a request or a byte from the modem arrives; then
 *            rbPoll() once for every line waiting (it reads one per call),
 *            hand any whole MT to the application and acknowledge it, and
 *            serve the request. The library's async path does nothing on a
 *            timer, so there is no polling and an idle modem costs no
 *            wakes.
 *   requests one slot. A caller takes it, fills it, kicks the worker and
 *            waits as long as it said. If it stops waiting, the request is
 *            abandoned: the worker finishes it, frees the slot itself, and
 *            a send's outcome still comes through mo_done.
 *
 * Two rules the library forces:
 *   - its async API with callbacks only: the blocking send and receive
 *     paths do nothing once callbacks are registered;
 *   - no modem queries while an MO is moving: receiveJspr() discards lines
 *     that do not match what it is waiting for, so a query would swallow
 *     the modem's request for the next segment and stall the send.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>

#include <errno.h>
#include <string.h>

/* rockblock_9704.h declares the library's private helpers static, so every
 * file that includes it gets "declared static but never defined" warnings.
 * GCC raises them at the end of the file, so the ignore has to stay on for
 * the whole of it. */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "rockblock_9704.h"
#include "imt_queue.h"

#include <rb9704/rb9704.h>

#include "rb9704_internal.h"

LOG_MODULE_REGISTER(rb9704, CONFIG_RB9704_LOG_LEVEL);

enum op { OP_BEGIN, OP_END, OP_SEND, OP_CANCEL, OP_INFO, OP_SIGNAL, OP_POWER };
enum req_state { REQ_PENDING, REQ_DONE, REQ_ABANDONED };

static struct {
	enum op op;
	int result;
	uint16_t topic;
	size_t len;
	bool on;
	struct rb9704_info info;
	enum req_state state;
} req;

static uint8_t mo_buf[CONFIG_RB9704_MESSAGE_MAX];

#define EV_REQ BIT(0)
#define EV_RX BIT(1)

static K_SEM_DEFINE(rb9704_slot, 1, 1);
static K_EVENT_DEFINE(rb9704_ev);
static K_SEM_DEFINE(rb9704_done, 0, 1);
static struct k_spinlock req_lock;

static struct rb9704_status st;
static K_MUTEX_DEFINE(rb9704_st_lock);

static const struct rb9704_callbacks *app;
static bool mt_waiting;
static bool mo_cancel_asked;	/* so the failure that follows reads -ECANCELED */

#if !IS_ENABLED(CONFIG_RB9704_POWER)
int rb9704_power_init(void)
{
	return -ENOTSUP;
}
int rb9704_power_set(bool on)
{
	ARG_UNUSED(on);
	return -ENOTSUP;
}
bool rb9704_power_on(void)
{
	return true;
}
bool rb9704_power_present(void)
{
	return false;
}
#endif

/* --- library callbacks: inside rbPoll(), on this thread ------------------ */

static void cb_mo_started(const uint16_t id)
{
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.mo_id = id;
	st.mo_started_ms = k_uptime_get();
	k_mutex_unlock(&rb9704_st_lock);
	LOG_DBG("MO accepted by the modem: id %u", id);
}

static void cb_mo_complete(const uint16_t id, const rbMsgStatus_t status)
{
	uint16_t topic;
	int result = status == RB_MSG_STATUS_OK ? 0 : mo_cancel_asked ? -ECANCELED : -EIO;

	mo_cancel_asked = false;
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.mo_in_flight = false;
	topic = st.mo_topic;
	if (result == 0) {
		st.mo_sent++;
	} else {
		st.mo_failed++;
	}
	k_mutex_unlock(&rb9704_st_lock);
	LOG_INF("MO %s: topic %u id %u",
		result == 0 ? "sent" : result == -ECANCELED ? "cancelled" : "failed", topic, id);
	if (app != NULL && app->mo_done != NULL) {
		app->mo_done(topic, id, result, app->user);
	}
}

static void cb_mt_complete(const uint16_t id, const rbMsgStatus_t status)
{
	if (status == RB_MSG_STATUS_OK) {
		mt_waiting = true;	/* delivered from the loop, outside rbPoll */
	} else {
		k_mutex_lock(&rb9704_st_lock, K_FOREVER);
		st.mt_failed++;
		k_mutex_unlock(&rb9704_st_lock);
		LOG_WRN("MT failed: id %u", id);
	}
}

static void cb_constellation(const jsprConstellationState_t *cs)
{
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.signal_known = true;
	st.visibility_known = true;
	st.constellation_visible = cs->constellationVisible;
	st.signal_bars = cs->signalBars;
	st.signal_level = cs->signalLevel;
	st.signal_ms = k_uptime_get();
	k_mutex_unlock(&rb9704_st_lock);
	LOG_DBG("signal: %u bars, visible %d", cs->signalBars, cs->constellationVisible);
	if (app != NULL && app->signal != NULL) {
		app->signal(cs->signalBars, cs->constellationVisible, app->user);
	}
}

static void cb_provisioning(const jsprMessageProvisioning_t *mp)
{
	uint8_t n;

	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.provisioning_known = mp->provisioningSet;
	n = MIN(mp->topicCount, RB9704_MAX_TOPICS);
	st.topic_count = n;
	for (int i = 0; i < n; i++) {
		st.topics[i].id = mp->provisioning[i].topicId;
		strncpy(st.topics[i].name, mp->provisioning[i].topicName,
			sizeof(st.topics[i].name) - 1);
	}
	k_mutex_unlock(&rb9704_st_lock);
	LOG_INF("provisioning: %u topic(s)", mp->topicCount);
	if (app != NULL && app->provisioning != NULL) {
		app->provisioning(st.topics, n, app->user);
	}
}

static const rbCallbacks_t callbacks = {
	.messageProvisioning = cb_provisioning,
	.moMessageComplete = cb_mo_complete,
	.mtMessageComplete = cb_mt_complete,
	.constellationState = cb_constellation,
	.moMessageStarted = cb_mo_started,
};

/* --- the requests, on this thread --------------------------------------- */

/* Two of the library's globals, as of the commit west.yml pins (v1.0.1);
 * neither is static there, and a bump of the pin should check they still
 * are not. messageProvisioningInfo is its provisioning cache (op_send).
 * moQueuedMessages counts the MOs it holds; rbBegin empties the queue but
 * leaves the count, and a count above zero makes the next send queue
 * without transmitting, so a begin after a stuck send would never send
 * again. */
extern jsprMessageProvisioning_t messageProvisioningInfo;
extern uint16_t moQueuedMessages;

static int op_begin(void)
{
	if (st.begun) {
		return -EALREADY;
	}
	if (rb9704_power_present() && !rb9704_power_on()) {
		int rc = rb9704_power_set(true);

		if (rc != 0) {
			return rc;
		}
		k_mutex_lock(&rb9704_st_lock, K_FOREVER);
		st.powered = true;
		k_mutex_unlock(&rb9704_st_lock);
	}
	rbRegisterCallbacks(&callbacks);
	if (!rbBegin("zephyr")) {	/* the port name is unused: the UART is in devicetree */
		return -EIO;
	}
	moQueuedMessages = 0;	/* rbBegin emptied the queue; this is the count of it */
	mo_cancel_asked = false;
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.begun = true;
	st.mo_in_flight = false;
	k_mutex_unlock(&rb9704_st_lock);
	return 0;
}

static int op_end(void)
{
	bool ok;

	if (!st.begun) {
		return -EALREADY;
	}
	ok = rbEnd();
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.begun = false;
	st.mo_in_flight = false;
	k_mutex_unlock(&rb9704_st_lock);
	return ok ? 0 : -EIO;
}

static bool topic_provisioned(uint16_t topic)
{
	for (int i = 0; i < st.topic_count; i++) {
		if (st.topics[i].id == topic) {
			return true;
		}
	}
	return false;
}

/* The library asks the modem for its provisioning at the first send and
 * keeps the answer for good, a miss included, so a modem provisioned after
 * that would refuse every topic until a reboot. When the topic is not in
 * what it kept, clear the cache and the library asks again: one free JSPR
 * round trip. */
static int op_send(void)
{
	bool known;

	if (!st.begun) {
		return -ENOTCONN;
	}
	if (st.mo_in_flight) {
		return -EBUSY;
	}
	known = false;
	for (int i = 0; messageProvisioningInfo.provisioningSet &&
			i < MIN(messageProvisioningInfo.topicCount, JSPR_MAX_TOPICS); i++) {
		known |= messageProvisioningInfo.provisioning[i].topicId == req.topic;
	}
	if (!known) {
		messageProvisioningInfo.provisioningSet = false;
	}
	if (!rbSendMessageAsync(req.topic, (const char *)mo_buf, req.len)) {
		if (st.provisioning_known && !topic_provisioned(req.topic)) {
			return -EACCES;
		}
		/* The modem refused a topic the cache said was fine: the cache
		 * may be stale, so the next send asks again. */
		messageProvisioningInfo.provisioningSet = false;
		return -EIO;
	}
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.mo_in_flight = true;
	st.mo_topic = req.topic;
	k_mutex_unlock(&rb9704_st_lock);
	return 0;
}

/* The modem answers a cancel with a 299 messageOriginateStatus, which the
 * library reads in rbPoll as a failed MO and removes from its queue; the
 * flag set here turns that failure into -ECANCELED in cb_mo_complete. */
static int op_cancel(void)
{
	if (!st.begun) {
		return -ENOTCONN;
	}
	if (!st.mo_in_flight) {
		return -ENOENT;
	}
	if (!rbCancelMessage(st.mo_topic, st.mo_id)) {
		return -EIO;
	}
	mo_cancel_asked = true;
	return 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	if (src != NULL) {
		strncpy(dst, src, cap - 1);
		dst[cap - 1] = '\0';
	} else {
		dst[0] = '\0';
	}
}

static int op_info(void)
{
	struct rb9704_info *i = &req.info;
	char *imei;

	if (!st.begun) {
		return -ENOTCONN;
	}
	if (st.mo_in_flight) {
		return -EBUSY;
	}
	/* Each is one JSPR round trip: free, milliseconds at 230400. */
	imei = rbGetImei();
	copy_str(i->imei, sizeof(i->imei), imei);
	copy_str(i->serial, sizeof(i->serial), rbGetSerialNumber());
	copy_str(i->hw_version, sizeof(i->hw_version), rbGetHwVersion());
	copy_str(i->fw_version, sizeof(i->fw_version), rbGetFirmwareVersion());
	copy_str(i->iccid, sizeof(i->iccid), rbGetIccid());
	i->board_temp_c = rbGetBoardTemp();
	i->card_present = rbGetCardPresent();
	i->sim_connected = rbGetSimConnected();
	return imei != NULL ? 0 : -EIO;
}

static int op_signal(void)
{
	int8_t bars;

	if (!st.begun) {
		return -ENOTCONN;
	}
	if (st.mo_in_flight) {
		return -EBUSY;
	}
	bars = rbGetSignal();
	if (bars < 0) {
		return -EIO;
	}
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	st.signal_known = true;
	st.signal_bars = (uint8_t)bars;
	st.signal_ms = k_uptime_get();
	k_mutex_unlock(&rb9704_st_lock);
	return bars;
}

static int op_power(void)
{
	int rc;

	if (!req.on && st.begun) {
		(void)op_end();
	}
	rc = rb9704_power_set(req.on);
	if (rc == 0) {
		k_mutex_lock(&rb9704_st_lock, K_FOREVER);
		st.powered = req.on;
		k_mutex_unlock(&rb9704_st_lock);
	}
	return rc;
}

static int dispatch(void)
{
	switch (req.op) {
	case OP_BEGIN: return op_begin();
	case OP_END: return op_end();
	case OP_SEND: return op_send();
	case OP_CANCEL: return op_cancel();
	case OP_INFO: return op_info();
	case OP_SIGNAL: return op_signal();
	case OP_POWER: return op_power();
	}
	return -EINVAL;
}

/* After dispatch: hand the result to a caller still waiting, or free the
 * slot for one that stopped. */
static void complete(void)
{
	k_spinlock_key_t key = k_spin_lock(&req_lock);
	bool abandoned = req.state == REQ_ABANDONED;

	if (!abandoned) {
		req.state = REQ_DONE;
		k_sem_give(&rb9704_done);
	}
	k_spin_unlock(&req_lock, key);
	if (abandoned) {
		if (req.op == OP_SEND && req.result != 0 && app != NULL && app->mo_done != NULL) {
			app->mo_done(req.topic, 0, req.result, app->user);
		}
		k_sem_give(&rb9704_slot);
	}
}

/* Caller side. The slot is held from here until the caller has read the
 * result, or until the worker finishes a request the caller gave up on.
 * Abandonment is reported apart from the result: a request can itself end
 * in -ETIMEDOUT (a begin whose modem never signals booted), and then the
 * slot is still the caller's to give back. */
static int submit(k_timeout_t timeout, bool *abandoned)
{
	k_spinlock_key_t key;
	int rc;

	req.state = REQ_PENDING;
	k_sem_reset(&rb9704_done);
	k_event_post(&rb9704_ev, EV_REQ);
	rc = k_sem_take(&rb9704_done, timeout);
	key = k_spin_lock(&req_lock);
	*abandoned = rc != 0 && req.state != REQ_DONE;
	if (*abandoned) {
		req.state = REQ_ABANDONED;
	}
	k_spin_unlock(&req_lock, key);
	return *abandoned ? 0 : req.result;
}

static int take_slot(k_timeout_t timeout)
{
	if (k_current_get() == rb9704_worker_tid_get()) {
		return -EDEADLK;	/* from a callback: the worker would wait on itself */
	}
	return k_sem_take(&rb9704_slot, timeout) == 0 ? 0 : -EAGAIN;
}

static int simple(enum op op, k_timeout_t timeout)
{
	bool abandoned;
	int rc = take_slot(timeout);

	if (rc != 0) {
		return rc;
	}
	req.op = op;
	rc = submit(timeout, &abandoned);
	if (abandoned) {
		return -ETIMEDOUT;
	}
	k_sem_give(&rb9704_slot);
	return rc;
}

/* --- the API -------------------------------------------------------------- */

void rb9704_set_callbacks(const struct rb9704_callbacks *cb)
{
	app = cb;
}

int rb9704_power(bool on)
{
	bool abandoned;
	int rc;

	if (!rb9704_power_present()) {
		return -ENOTSUP;
	}
	rc = take_slot(K_FOREVER);
	if (rc != 0) {
		return rc;
	}
	req.op = OP_POWER;
	req.on = on;
	rc = submit(K_FOREVER, &abandoned);
	if (abandoned) {
		return -ETIMEDOUT;
	}
	k_sem_give(&rb9704_slot);
	return rc;
}

int rb9704_begin(k_timeout_t timeout)
{
	return simple(OP_BEGIN, timeout);
}

int rb9704_end(k_timeout_t timeout)
{
	return simple(OP_END, timeout);
}

int rb9704_signal(k_timeout_t timeout)
{
	return simple(OP_SIGNAL, timeout);
}

int rb9704_cancel(k_timeout_t timeout)
{
	return simple(OP_CANCEL, timeout);
}

int rb9704_info(struct rb9704_info *out, k_timeout_t timeout)
{
	bool abandoned;
	int rc = take_slot(timeout);

	if (rc != 0) {
		return rc;
	}
	req.op = OP_INFO;
	rc = submit(timeout, &abandoned);
	if (abandoned) {
		return -ETIMEDOUT;
	}
	if (rc == 0) {
		*out = req.info;
	}
	k_sem_give(&rb9704_slot);
	return rc;
}

int rb9704_send(uint16_t topic, const void *data, size_t len, k_timeout_t timeout)
{
	bool abandoned;
	int rc;

	if (topic < RB9704_TOPIC_MIN || data == NULL || len == 0 || len > sizeof(mo_buf)) {
		return -EINVAL;
	}
	rc = take_slot(timeout);
	if (rc != 0) {
		return rc;
	}
	req.op = OP_SEND;
	req.topic = topic;
	req.len = len;
	memcpy(mo_buf, data, len);
	rc = submit(timeout, &abandoned);
	if (abandoned) {
		return -EINPROGRESS;
	}
	k_sem_give(&rb9704_slot);
	return rc;
}

void rb9704_status(struct rb9704_status *out)
{
	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	*out = st;
	k_mutex_unlock(&rb9704_st_lock);
	out->rx_dropped = rb9704_serial_dropped();
}

void rb9704_spy(bool on)
{
	rb9704_serial_spy(on);
}

/* --- the loop -------------------------------------------------------------- */

/* Every whole MT in the library's queue, to the application, then
 * acknowledged so the modem can send the next. */
static void deliver_mts(void)
{
	char *payload = NULL;
	size_t len;

	while ((len = rbReceiveMessageAsync(&payload)) > 0 && payload != NULL) {
		imt_t *head = imtQueueMtGetFirst();
		struct rb9704_mt mt = {
			.topic = head != NULL ? head->topic : 0,
			.id = head != NULL ? head->id : 0,
			.data = (const uint8_t *)payload,
			.len = len,
		};

		k_mutex_lock(&rb9704_st_lock, K_FOREVER);
		st.mt_received++;
		k_mutex_unlock(&rb9704_st_lock);
		LOG_INF("MT received: topic %u id %u, %u bytes", mt.topic, mt.id,
			(unsigned int)len);
		if (app != NULL && app->mt != NULL) {
			app->mt(&mt, app->user);
		}
		rbAcknowledgeReceiveHeadAsync();
		payload = NULL;
	}
}

static void worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	k_mutex_lock(&rb9704_st_lock, K_FOREVER);
	/* No pins: the board is powered whenever this runs. */
	st.powered = !rb9704_power_present();
	if (rb9704_power_present() && rb9704_power_init() != 0) {
		LOG_ERR("power pins not ready");
	}
	k_mutex_unlock(&rb9704_st_lock);

	for (;;) {
		uint32_t ev = k_event_wait(&rb9704_ev, EV_REQ | EV_RX, false, K_FOREVER);

		/* Cleared before the work, so a byte or request that arrives
		 * during it wakes the next wait. */
		k_event_clear(&rb9704_ev, ev);
		if (st.begun) {
			while (rb9704_serial_available() > 0) {
				rbPoll();	/* one JSPR line per call */
			}
			if (mt_waiting) {
				mt_waiting = false;
				deliver_mts();
			}
		}
		if (ev & EV_REQ) {
			req.result = dispatch();
			complete();
		}
	}
}

K_THREAD_DEFINE(rb9704_worker_tid, CONFIG_RB9704_WORKER_STACK_SIZE, worker, NULL, NULL, NULL,
		CONFIG_RB9704_WORKER_PRIORITY, 0, 0);

k_tid_t rb9704_worker_tid_get(void)
{
	return rb9704_worker_tid;
}

/* From the UART ISR: bytes have arrived for the library. */
void rb9704_worker_rx(void)
{
	k_event_post(&rb9704_ev, EV_RX);
}
