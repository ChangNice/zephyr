/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth HID Profile – shared utility functions.
 *
 * Functions in this file are used by both hid_device.c and hid_host.c.
 * This file is compiled whenever CONFIG_BT_HID is enabled, regardless of
 * which role (Device / Host) is selected.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/classic/hid.h>

#include "hid_internal.h"

#define LOG_MODULE_NAME bt_hid_common
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_BT_HID_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Shared TX buffer pool
 *
 * Both the Device role (hid_device.c) and the Host role (hid_host.c) draw
 * from this pool.  Pool size is configured by:
 *   CONFIG_BT_HID_TX_BUF_COUNT  – number of buffers
 *   CONFIG_BT_HID_TX_BUF_SIZE   – maximum payload per buffer
 * -------------------------------------------------------------------------
 */

NET_BUF_POOL_FIXED_DEFINE(hid_tx_pool,
			  CONFIG_BT_HID_TX_BUF_COUNT,
			  BT_L2CAP_SDU_BUF_SIZE(CONFIG_BT_HID_TX_BUF_SIZE),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE,
			  NULL);

struct net_buf *bt_hid_alloc_buf(size_t reserve)
{
	struct net_buf *buf;

	buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
	if (!buf) {
		LOG_WRN("No HID TX buffer available");
		return NULL;
	}
	net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE + reserve);
	return buf;
}

/* -------------------------------------------------------------------------
 * Common 1-byte Control channel send
 * -------------------------------------------------------------------------
 */

int bt_hid_send_ctrl_byte(struct bt_hid_conn *conn, uint8_t hdr)
{
	struct net_buf *buf;
	int err;

	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_add_u8(buf, hdr);

	err = bt_l2cap_chan_send(&conn->ctrl_chan.chan, buf);
	if (err < 0) {
		LOG_ERR("ctrl send failed: %d", err);
		net_buf_unref(buf);
		return -EIO;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Common disconnect helper (Interrupt first, then Control, Spec §5.2.2)
 *
 * Called from both hid_device.c and hid_host.c.
 * The disconnected callbacks in each role's file call the role-specific
 * conn_free() function after the channels close.
 * -------------------------------------------------------------------------
 */

void bt_hid_conn_close(struct bt_hid_conn *conn)
{
	if (atomic_test_and_set_bit(&conn->flags, BT_HID_FLAG_INTR_DISC_PEND)) {
		/* Already in progress */
		return;
	}

	if (atomic_test_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED)) {
		conn->state = BT_HID_INTR_DISCONNECTING;
		bt_l2cap_chan_disconnect(&conn->intr_chan.chan);
	} else if (atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		conn->state = BT_HID_CTRL_DISCONNECTING;
		bt_l2cap_chan_disconnect(&conn->ctrl_chan.chan);
	}
	/* If neither channel is connected, the role-specific disconnected
	 * callback will have already freed the connection object. */
}

/* -------------------------------------------------------------------------
 * Public utility accessors (declared in hid.h)
 * -------------------------------------------------------------------------
 */

struct bt_conn *bt_hid_conn_get_acl(const struct bt_hid_conn *conn)
{
	if (!conn) {
		return NULL;
	}
	return conn->acl;
}

enum bt_hid_conn_state bt_hid_conn_get_state(const struct bt_hid_conn *conn)
{
	if (!conn) {
		return BT_HID_DISCONNECTED;
	}
	return conn->state;
}
