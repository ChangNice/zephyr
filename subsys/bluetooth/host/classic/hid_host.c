/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth HID Host Role implementation.
 *
 * Implements the HID Host role of the Bluetooth HID Profile v1.1.2.
 * The Host initiates connections to HID Devices by opening:
 *   1. Control channel (PSM 0x0011) first
 *   2. Interrupt channel (PSM 0x0013) after Control is fully configured
 *
 * Host-initiated transfers (GET_REPORT, SET_REPORT, GET_PROTOCOL,
 * SET_PROTOCOL, HID_CONTROL) are sent on the Control channel.
 * Asynchronous Output reports are sent on the Interrupt channel.
 * Asynchronous Input reports from the Device arrive on the Interrupt channel.
 *
 * Spec reference: HID Profile v1.1.2, Bluetooth SIG.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/classic/hid.h>

#include "hid_internal.h"

#define LOG_MODULE_NAME bt_hid_host
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_BT_HID_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Connection pool
 * -------------------------------------------------------------------------
 */

static struct bt_hid_conn hid_host_pool[CONFIG_BT_HID_HOST_MAX_CONN];

/*
 * Work item used to open the Interrupt channel after the Control channel is
 * fully configured.  We must NOT call bt_l2cap_chan_connect() synchronously
 * from inside the L2CAP connected callback because the L2CAP signaling state
 * machine is still processing the ctrl-channel configuration exchange at that
 * point.  Calling connect() there causes the new request's ident to collide
 * with the in-flight conf-rsp ident, producing an "ident mismatch" warning
 * and a connection timeout.
 */
static struct k_work_delayable hid_host_open_intr_work;
static struct bt_hid_conn     *hid_host_intr_pending;

/* -------------------------------------------------------------------------
 * Registered callbacks
 * -------------------------------------------------------------------------
 */

static const struct bt_hid_host_cb *hid_host_cb;

/* -------------------------------------------------------------------------
 * Connection pool management
 * -------------------------------------------------------------------------
 */

static struct bt_hid_conn *hid_host_conn_new(struct bt_conn *acl)
{
	for (int i = 0; i < ARRAY_SIZE(hid_host_pool); i++) {
		struct bt_hid_conn *c = &hid_host_pool[i];

		if (c->acl == NULL) {
			memset(c, 0, sizeof(*c));
			c->acl      = bt_conn_ref(acl);
			c->state    = BT_HID_DISCONNECTED;
			c->protocol = BT_HID_PROTO_REPORT_MODE;
			atomic_clear(&c->flags);
			atomic_set_bit(&c->flags, BT_HID_FLAG_HOST_ROLE);
			k_sem_init(&c->ctrl_tx_sem, 1, 1);
			return c;
		}
	}
	return NULL;
}

static void hid_host_conn_free(struct bt_hid_conn *conn)
{
	if (conn->acl) {
		bt_conn_unref(conn->acl);
		conn->acl = NULL;
	}
	conn->state = BT_HID_DISCONNECTED;
}

/* -------------------------------------------------------------------------
 * Disconnect helper (Spec §5.2.2: Interrupt first, then Control)
 * -------------------------------------------------------------------------
 */

static void hid_host_conn_close(struct bt_hid_conn *conn)
{
	if (atomic_test_and_set_bit(&conn->flags, BT_HID_FLAG_INTR_DISC_PEND)) {
		return;
	}

	if (atomic_test_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED)) {
		conn->state = BT_HID_INTR_DISCONNECTING;
		bt_l2cap_chan_disconnect(&conn->intr_chan.chan);
	} else if (atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		conn->state = BT_HID_CTRL_DISCONNECTING;
		bt_l2cap_chan_disconnect(&conn->ctrl_chan.chan);
	} else {
		hid_host_conn_free(conn);
	}
}

/* -------------------------------------------------------------------------
 * Control TX serialisation helpers
 *
 * Spec §3.2.1: Host must not have more than one Control channel transfer
 * simultaneously outstanding to a given HID Device.
 * -------------------------------------------------------------------------
 */

static int hid_host_ctrl_send(struct bt_hid_conn *conn, struct net_buf *buf)
{
	int err;

	if (k_sem_take(&conn->ctrl_tx_sem, K_NO_WAIT) != 0) {
		net_buf_unref(buf);
		return -EBUSY;
	}

	atomic_set_bit(&conn->flags, BT_HID_FLAG_CTRL_TX_PENDING);

	err = bt_l2cap_chan_send(&conn->ctrl_chan.chan, buf);
	if (err < 0) {
		LOG_ERR("ctrl send failed: %d", err);
		net_buf_unref(buf);
		atomic_clear_bit(&conn->flags, BT_HID_FLAG_CTRL_TX_PENDING);
		k_sem_give(&conn->ctrl_tx_sem);
		return -EIO;
	}
	return 0;
}

static void hid_host_ctrl_tx_done(struct bt_hid_conn *conn)
{
	if (atomic_test_and_clear_bit(&conn->flags,
				      BT_HID_FLAG_CTRL_TX_PENDING)) {
		k_sem_give(&conn->ctrl_tx_sem);
	}
}

/* -------------------------------------------------------------------------
 * HIDP message handlers (Control channel, Device → Host)
 * -------------------------------------------------------------------------
 */

static void hid_host_handle_handshake(struct bt_hid_conn *conn, uint8_t hdr)
{
	uint8_t result_code = bt_hidp_param(hdr);

	LOG_DBG("HANDSHAKE result=0x%02x", result_code);
	hid_host_ctrl_tx_done(conn);

	if (hid_host_cb && hid_host_cb->handshake) {
		hid_host_cb->handshake(conn, result_code);
	}
}

static void hid_host_handle_ctrl_data(struct bt_hid_conn *conn,
				      uint8_t hdr, struct net_buf *buf)
{
	uint8_t report_type = bt_hidp_param(hdr) & 0x03u;

	LOG_DBG("ctrl DATA type=%u len=%u", report_type, buf->len);
	hid_host_ctrl_tx_done(conn);

	if (report_type == BT_HID_REPORT_TYPE_OTHER) {
		/*
		 * GET_PROTOCOL response: single-byte payload = protocol mode
		 * (Spec §3.1.2.5, Table 3.7).
		 */
		if (buf->len >= 1u) {
			uint8_t mode = net_buf_pull_u8(buf);

			conn->protocol = mode;
			LOG_DBG("GET_PROTOCOL response: mode=%u", mode);
			if (hid_host_cb && hid_host_cb->protocol_mode) {
				hid_host_cb->protocol_mode(conn, mode);
			}
		}
	} else {
		/* GET_REPORT response */
		if (hid_host_cb && hid_host_cb->get_report_rsp) {
			hid_host_cb->get_report_rsp(conn, report_type,
						    buf->data, buf->len);
		}
	}
}

static void hid_host_handle_hid_control(struct bt_hid_conn *conn, uint8_t hdr)
{
	uint8_t op = bt_hidp_param(hdr);

	LOG_DBG("HID_CONTROL from Device op=0x%02x", op);

	/*
	 * Spec §3.1.2.2.3: Host shall ignore all HID_CONTROL messages from
	 * Device except VIRTUAL_CABLE_UNPLUG.
	 */
	if (op != BT_HIDP_CTRL_VIRTUAL_CABLE_UNPLUG) {
		LOG_DBG("Ignoring HID_CONTROL op 0x%02x from Device", op);
		return;
	}

	/*
	 * Respond by disconnecting Interrupt first, then Control.
	 * No HANDSHAKE is sent (Spec §3.1.2.2.3).
	 */
	atomic_set_bit(&conn->flags, BT_HID_FLAG_VC_UNPLUG);

	if (hid_host_cb && hid_host_cb->virtual_cable_unplug) {
		hid_host_cb->virtual_cable_unplug(conn);
	}

	hid_host_conn_close(conn);
}

/* -------------------------------------------------------------------------
 * L2CAP channel ops – Control channel
 * -------------------------------------------------------------------------
 */

static void hid_host_open_intr_handler(struct k_work *work)
{
	struct bt_hid_conn *conn = hid_host_intr_pending;

	hid_host_intr_pending = NULL;

	if (!conn || !conn->acl) {
		return;
	}

	if (bt_l2cap_chan_connect(conn->acl, &conn->intr_chan.chan,
				  BT_HID_PSM_INTR) < 0) {
		LOG_ERR("Failed to open HID Host intr channel");
		bt_l2cap_chan_disconnect(&conn->ctrl_chan.chan);
	}
}

static void hid_host_ctrl_connected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, ctrl_chan.chan);

	LOG_DBG("HID Host ctrl connected");
	atomic_set_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED);
	conn->state = BT_HID_INTR_CONNECTING;
	conn->intr_chan.rx.mtu = BT_HID_MTU_REPORT;

	/*
	 * Defer opening the Interrupt channel to a work item.
	 * Calling bt_l2cap_chan_connect() synchronously here would send a
	 * new L2CAP signaling request while the stack is still processing the
	 * ctrl-channel CONFIG exchange, causing an ident collision that results
	 * in "ident mismatch" warnings and a connection timeout.
	 */
	hid_host_intr_pending = conn;
	k_work_reschedule(&hid_host_open_intr_work, K_NO_WAIT);
}

static void hid_host_ctrl_disconnected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, ctrl_chan.chan);
	uint8_t reason = conn->disconnect_reason;

	LOG_DBG("HID Host ctrl disconnected");
	atomic_clear_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED);

	/* Release TX semaphore in case we were awaiting a response */
	hid_host_ctrl_tx_done(conn);

	if (conn->state == BT_HID_CTRL_DISCONNECTING ||
	    conn->state == BT_HID_CONNECTED ||
	    conn->state == BT_HID_SUSPENDED) {
		if (hid_host_cb && hid_host_cb->disconnected) {
			hid_host_cb->disconnected(conn, reason);
		}
		hid_host_conn_free(conn);
	}
}

static int hid_host_ctrl_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, ctrl_chan.chan);
	uint8_t hdr;
	uint8_t msg_type;

	if (buf->len < 1u) {
		LOG_WRN("Host ctrl recv: empty buffer");
		return 0;
	}

	hdr      = net_buf_pull_u8(buf);
	msg_type = bt_hidp_msg_type(hdr);

	LOG_DBG("host ctrl recv hdr=0x%02x", hdr);

	switch (msg_type) {
	case BT_HIDP_HANDSHAKE:
		hid_host_handle_handshake(conn, hdr);
		break;
	case BT_HIDP_DATA:
		hid_host_handle_ctrl_data(conn, hdr, buf);
		break;
	case BT_HIDP_HID_CONTROL:
		hid_host_handle_hid_control(conn, hdr);
		break;
	default:
		LOG_WRN("Unexpected HIDP msg 0x%02x from Device on ctrl",
			msg_type);
		break;
	}

	return 0;
}

static const struct bt_l2cap_chan_ops hid_host_ctrl_ops = {
	.connected    = hid_host_ctrl_connected,
	.disconnected = hid_host_ctrl_disconnected,
	.recv         = hid_host_ctrl_recv,
};

/* -------------------------------------------------------------------------
 * L2CAP channel ops – Interrupt channel
 * -------------------------------------------------------------------------
 */

static void hid_host_intr_connected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, intr_chan.chan);

	LOG_DBG("HID Host intr connected");
	atomic_set_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED);
	conn->state = BT_HID_CONNECTED;

	if (hid_host_cb && hid_host_cb->connected) {
		hid_host_cb->connected(conn);
	}
}

static void hid_host_intr_disconnected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, intr_chan.chan);

	LOG_DBG("HID Host intr disconnected");
	atomic_clear_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED);

	/* Spec §5.2.2: after Interrupt closes, close Control */
	if (atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		conn->state = BT_HID_CTRL_DISCONNECTING;
		bt_l2cap_chan_disconnect(&conn->ctrl_chan.chan);
	} else {
		if (hid_host_cb && hid_host_cb->disconnected) {
			hid_host_cb->disconnected(conn,
						  conn->disconnect_reason);
		}
		hid_host_conn_free(conn);
	}
}

static int hid_host_intr_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, intr_chan.chan);
	uint8_t hdr;
	uint8_t msg_type;
	uint8_t report_type;

	if (buf->len < 1u) {
		return 0;
	}

	hdr         = net_buf_pull_u8(buf);
	msg_type    = bt_hidp_msg_type(hdr);
	report_type = bt_hidp_param(hdr) & 0x03u;

	/*
	 * Spec §3.2.2: only DATA messages are valid on the Interrupt channel.
	 * All DATA payloads Device → Host are Input reports.
	 */
	if (msg_type != BT_HIDP_DATA) {
		LOG_WRN("Non-DATA on Interrupt channel (0x%02x) – ignored",
			msg_type);
		return 0;
	}

	LOG_DBG("intr recv input report type=%u len=%u",
		report_type, buf->len);

	if (hid_host_cb && hid_host_cb->input_report) {
		hid_host_cb->input_report(conn, report_type,
					  buf->data, buf->len);
	}

	return 0;
}

static const struct bt_l2cap_chan_ops hid_host_intr_ops = {
	.connected    = hid_host_intr_connected,
	.disconnected = hid_host_intr_disconnected,
	.recv         = hid_host_intr_recv,
};

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------
 */

int bt_hid_host_register(const struct bt_hid_host_cb *cb)
{
	if (!cb) {
		return -EINVAL;
	}

	if (hid_host_cb != NULL) {
		return -EALREADY;
	}

	k_work_init_delayable(&hid_host_open_intr_work, hid_host_open_intr_handler);
	hid_host_cb = cb;
	LOG_INF("HID Host registered");
	return 0;
}

struct bt_hid_conn *bt_hid_host_connect(struct bt_conn *conn)
{
	struct bt_hid_conn *hid_conn;
	int err;

	if (!conn) {
		return NULL;
	}

	hid_conn = hid_host_conn_new(conn);
	if (!hid_conn) {
		LOG_ERR("No free HID Host connection slots");
		return NULL;
	}

	hid_conn->ctrl_chan.chan.ops = &hid_host_ctrl_ops;
	hid_conn->ctrl_chan.rx.mtu  = BT_HID_MTU_REPORT;
	hid_conn->intr_chan.chan.ops = &hid_host_intr_ops;
	hid_conn->intr_chan.rx.mtu  = BT_HID_MTU_REPORT;
	hid_conn->state             = BT_HID_CTRL_CONNECTING;

	/* Open Control channel first (Spec §5.2.2) */
	err = bt_l2cap_chan_connect(conn, &hid_conn->ctrl_chan.chan,
				    BT_HID_PSM_CTRL);
	if (err < 0) {
		LOG_ERR("Failed to connect HID control channel: %d", err);
		hid_host_conn_free(hid_conn);
		return NULL;
	}

	LOG_DBG("HID Host connecting ctrl channel");
	return hid_conn;
}

int bt_hid_host_get_report(struct bt_hid_conn *conn, uint8_t type,
			   uint8_t id, uint16_t buf_size)
{
	struct net_buf *buf;
	uint8_t hdr;
	bool has_id;
	bool has_size;

	if (!conn) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}
	if (type == BT_HID_REPORT_TYPE_RESERVED) {
		return -EINVAL;
	}

	/*
	 * GET_REPORT format (Spec §3.1.2.3, Table 3.4):
	 *   HIDP-Hdr[7:4] = 4 (GET_REPORT)
	 *   HIDP-Hdr[3]   = Size (1 if BufferSize follows)
	 *   HIDP-Hdr[2]   = reserved (0)
	 *   HIDP-Hdr[1:0] = ReportType
	 */
	has_id   = (id != 0u) || (conn->protocol == BT_HID_PROTO_BOOT_MODE);
	has_size = (buf_size > 0u);

	hdr = bt_hidp_hdr(BT_HIDP_GET_REPORT,
			  (uint8_t)((has_size ? 0x08u : 0x00u) | (type & 0x03u)));

	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}

	net_buf_add_u8(buf, hdr);
	if (has_id) {
		net_buf_add_u8(buf, id);
	}
	if (has_size) {
		net_buf_add_le16(buf, buf_size);
	}

	LOG_DBG("GET_REPORT type=%u id=%u buf_size=%u", type, id, buf_size);
	return hid_host_ctrl_send(conn, buf);
}

int bt_hid_host_set_report(struct bt_hid_conn *conn, uint8_t type,
			   const uint8_t *data, uint16_t len)
{
	struct net_buf *buf;
	uint8_t hdr;
	uint16_t ctrl_mtu;

	if (!conn || !data) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}
	if (type == BT_HID_REPORT_TYPE_RESERVED) {
		return -EINVAL;
	}

	ctrl_mtu = conn->ctrl_chan.tx.mtu;

	/*
	 * Spec §3.1.2.4: if payload plus header equals or exceeds MTU,
	 * the Host shall not send the report.
	 */
	if ((uint32_t)len + 1u >= ctrl_mtu) {
		LOG_WRN("SET_REPORT payload too large: %u >= MTU %u",
			len + 1u, ctrl_mtu);
		return -EMSGSIZE;
	}

	hdr = bt_hidp_hdr(BT_HIDP_SET_REPORT, type & 0x03u);

	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}

	net_buf_add_u8(buf, hdr);
	net_buf_add_mem(buf, data, len);

	LOG_DBG("SET_REPORT type=%u len=%u", type, len);
	return hid_host_ctrl_send(conn, buf);
}

int bt_hid_host_get_protocol(struct bt_hid_conn *conn)
{
	struct net_buf *buf;
	uint8_t hdr;

	if (!conn) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}

	hdr = bt_hidp_hdr(BT_HIDP_GET_PROTOCOL, 0x00u);

	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_add_u8(buf, hdr);

	LOG_DBG("GET_PROTOCOL");
	return hid_host_ctrl_send(conn, buf);
}

int bt_hid_host_set_protocol(struct bt_hid_conn *conn, uint8_t protocol)
{
	struct net_buf *buf;
	uint8_t hdr;

	if (!conn) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}
	if (protocol != BT_HID_PROTO_BOOT_MODE &&
	    protocol != BT_HID_PROTO_REPORT_MODE) {
		return -EINVAL;
	}

	hdr = bt_hidp_hdr(BT_HIDP_SET_PROTOCOL, protocol & 0x01u);

	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_add_u8(buf, hdr);

	LOG_DBG("SET_PROTOCOL -> %u", protocol);
	return hid_host_ctrl_send(conn, buf);
}

int bt_hid_host_send_output_report(struct bt_hid_conn *conn,
				   const uint8_t *data, uint16_t len)
{
	struct net_buf *buf;
	uint8_t hdr;
	int err;
	uint16_t intr_mtu;

	if (!conn || !data) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED)) {
		return -ENOTCONN;
	}

	intr_mtu = conn->intr_chan.tx.mtu;

	/* Spec §3.2.2.2: if payload would exceed MTU, do not send */
	if ((uint32_t)len + 1u > intr_mtu) {
		LOG_WRN("Output report (%u B) exceeds intr MTU (%u)",
			len + 1u, intr_mtu);
		return -EMSGSIZE;
	}

	hdr = bt_hidp_hdr(BT_HIDP_DATA, BT_HID_REPORT_TYPE_OUTPUT);

	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_add_u8(buf, hdr);
	net_buf_add_mem(buf, data, len);

	err = bt_l2cap_chan_send(&conn->intr_chan.chan, buf);
	if (err < 0) {
		LOG_ERR("intr output report send failed: %d", err);
		net_buf_unref(buf);
		return -EIO;
	}

	LOG_DBG("Output report sent len=%u", len);
	return 0;
}

int bt_hid_host_send_control(struct bt_hid_conn *conn, uint8_t operation)
{
	struct net_buf *buf;
	uint8_t hdr;
	int err;

	if (!conn) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}

	/*
	 * Only SUSPEND, EXIT_SUSPEND, and VIRTUAL_CABLE_UNPLUG are valid
	 * HID_CONTROL operations from a Host (Spec §3.1.2.2).
	 */
	switch (operation) {
	case BT_HIDP_CTRL_SUSPEND:
	case BT_HIDP_CTRL_EXIT_SUSPEND:
	case BT_HIDP_CTRL_VIRTUAL_CABLE_UNPLUG:
		break;
	default:
		LOG_WRN("Invalid HID_CONTROL op 0x%02x for Host", operation);
		return -EINVAL;
	}

	hdr = bt_hidp_hdr(BT_HIDP_HID_CONTROL, operation);

	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_add_u8(buf, hdr);

	/*
	 * HID_CONTROL does not generate a HANDSHAKE response (Spec §3.2.1.3),
	 * so do NOT use hid_host_ctrl_send (which acquires the semaphore).
	 */
	err = bt_l2cap_chan_send(&conn->ctrl_chan.chan, buf);
	if (err < 0) {
		LOG_ERR("HID_CONTROL send failed: %d", err);
		net_buf_unref(buf);
		return -EIO;
	}

	/*
	 * Post-send state transitions (Spec §3.1.2.2.2 and §3.1.2.2.3).
	 */
	if (operation == BT_HIDP_CTRL_VIRTUAL_CABLE_UNPLUG) {
		atomic_set_bit(&conn->flags, BT_HID_FLAG_VC_UNPLUG);
		if (hid_host_cb && hid_host_cb->virtual_cable_unplug) {
			hid_host_cb->virtual_cable_unplug(conn);
		}
		hid_host_conn_close(conn);
	} else if (operation == BT_HIDP_CTRL_SUSPEND) {
		conn->state = BT_HID_SUSPENDED;
	} else if (operation == BT_HIDP_CTRL_EXIT_SUSPEND) {
		if (conn->state == BT_HID_SUSPENDED) {
			conn->state = BT_HID_CONNECTED;
		}
	}

	LOG_DBG("HID_CONTROL op=0x%02x sent", operation);
	return 0;
}

int bt_hid_host_disconnect(struct bt_hid_conn *conn)
{
	if (!conn) {
		return -EINVAL;
	}
	if (conn->state == BT_HID_DISCONNECTED) {
		return -ENOTCONN;
	}

	hid_host_conn_close(conn);
	return 0;
}
