/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Internal HID Profile definitions shared between hid_device.c and
 *        hid_host.c.  Not part of the public API.
 */

#ifndef ZEPHYR_SUBSYS_BLUETOOTH_HOST_CLASSIC_HID_INTERNAL_H_
#define ZEPHYR_SUBSYS_BLUETOOTH_HOST_CLASSIC_HID_INTERNAL_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/slist.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/classic/hid.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * MTU values (Spec §5.2.3.1 and §5.2.4.1)
 * -------------------------------------------------------------------------
 */

/**
 * Minimum L2CAP MTU for Boot Protocol Mode hosts (Spec §5.2.3.1).
 * Also the minimum MTU mandated by the Bluetooth Core Spec.
 */
#define BT_HID_MIN_MTU_BOOT    48u

/**
 * Recommended L2CAP MTU for Report Protocol Mode hosts (Spec §5.2.3.1).
 */
#define BT_HID_MTU_REPORT      672u

/**
 * Maximum boot protocol report size including Report ID (Spec §3.3.2).
 * Reports must fit within the minimum 48-octet MTU minus the 2-octet
 * L2CAP overhead (MTU – 1 HIDP hdr – 1 padding = 46).
 */
#define BT_HID_BOOT_REPORT_MAX_SIZE  46u

/* -------------------------------------------------------------------------
 * Connection flags (atomic bit positions)
 * -------------------------------------------------------------------------
 */

/** Control channel is connected */
#define BT_HID_FLAG_CTRL_CONNECTED   0
/** Interrupt channel is connected */
#define BT_HID_FLAG_INTR_CONNECTED   1
/** Virtual Cable feature active on this connection */
#define BT_HID_FLAG_VIRTUAL_CABLE    2
/** A Control-channel transfer is currently outstanding */
#define BT_HID_FLAG_CTRL_TX_PENDING  3
/** Connection is used by the Host role (vs Device role) */
#define BT_HID_FLAG_HOST_ROLE        4
/** Virtual Cable Unplug sequence in progress */
#define BT_HID_FLAG_VC_UNPLUG        5
/** Interrupt channel disconnect requested (pending ctrl disconnect) */
#define BT_HID_FLAG_INTR_DISC_PEND   6

/* -------------------------------------------------------------------------
 * Internal HID connection structure
 *
 * The Control and Interrupt channels each embed a bt_l2cap_br_chan.
 * The bt_l2cap_br_chan contains a bt_l2cap_chan as its first member,
 * so CONTAINER_OF can recover the enclosing bt_hid_conn.
 *
 * Layout note:
 *   ctrl_chan MUST be the very first member so that a pointer to
 *   bt_hid_conn is equivalent to a pointer to its ctrl_chan.chan
 *   from the L2CAP accept callback's perspective.  The intr_chan
 *   is found via CONTAINER_OF from its own L2CAP callbacks.
 * -------------------------------------------------------------------------
 */

/**
 * @brief Internal HID connection object.
 *
 * Allocated from a static pool in hid_device.c or hid_host.c.
 */
struct bt_hid_conn {
	/**
	 * Control channel (PSM 0x0011).
	 * Must be first – L2CAP accept callbacks receive a pointer to this
	 * chan and we recover bt_hid_conn via CONTAINER_OF.
	 */
	struct bt_l2cap_br_chan ctrl_chan;

	/**
	 * Interrupt channel (PSM 0x0013).
	 * Opened after the Control channel is fully configured.
	 */
	struct bt_l2cap_br_chan intr_chan;

	/** Underlying ACL connection (bt_conn_ref/unref managed internally) */
	struct bt_conn *acl;

	/** Current connection state */
	enum bt_hid_conn_state state;

	/**
	 * Atomic flags – use BT_HID_FLAG_* bit positions with
	 * atomic_set_bit / atomic_clear_bit / atomic_test_bit.
	 */
	atomic_t flags;

	/**
	 * Current HID protocol mode.
	 * BT_HID_PROTO_REPORT_MODE (default) or BT_HID_PROTO_BOOT_MODE.
	 * Only meaningful when state >= BT_HID_CONNECTED.
	 */
	uint8_t protocol;

	/**
	 * Semaphore serialising Control-channel transmissions.
	 *
	 * The HID specification (§3.2.1) requires that a Host must not have
	 * more than one Control-channel transfer simultaneously outstanding.
	 * This semaphore (initial count 1) is taken before sending a request
	 * and given back when the corresponding response is received.
	 */
	struct k_sem ctrl_tx_sem;

	/** HCI disconnect reason recorded at disconnection time */
	uint8_t disconnect_reason;

	/** Intrusive list node (used by pool tracking if needed) */
	sys_snode_t node;
};

/* -------------------------------------------------------------------------
 * Helper: build a 1-byte HIDP header octet
 *
 *   bits [7:4] = message type
 *   bits [3:0] = parameter
 * -------------------------------------------------------------------------
 */
static inline uint8_t bt_hidp_hdr(uint8_t msg_type, uint8_t param)
{
	return (uint8_t)((msg_type & 0xF0u) | (param & 0x0Fu));
}

/* -------------------------------------------------------------------------
 * Helper: extract message type from a HIDP header byte
 * -------------------------------------------------------------------------
 */
static inline uint8_t bt_hidp_msg_type(uint8_t hdr)
{
	return (uint8_t)(hdr & 0xF0u);
}

/* -------------------------------------------------------------------------
 * Helper: extract parameter field from a HIDP header byte
 * -------------------------------------------------------------------------
 */
static inline uint8_t bt_hidp_param(uint8_t hdr)
{
	return (uint8_t)(hdr & 0x0Fu);
}

/* -------------------------------------------------------------------------
 * Shared internal functions (implemented in hid_device.c / hid_host.c
 * but declared here so they can be called from helper translation units
 * if the build is ever split further)
 * -------------------------------------------------------------------------
 */

/**
 * @brief Send a single-byte HIDP message on the Control channel.
 *
 * Allocates a net_buf, writes the 1-octet header, and calls
 * bt_l2cap_chan_send.
 *
 * @param conn HID connection.
 * @param hdr  Pre-built HIDP header byte.
 *
 * @retval 0       Success.
 * @retval -ENOMEM No buffer available.
 * @retval -EIO    L2CAP send failure.
 */
int bt_hid_send_ctrl_byte(struct bt_hid_conn *conn, uint8_t hdr);

/**
 * @brief Allocate a net_buf suitable for an L2CAP BR/EDR send.
 *
 * Reserves L2CAP + HIDP header headroom.
 *
 * @param reserve Extra headroom in bytes beyond L2CAP headers.
 *
 * @return Pointer to allocated net_buf, or NULL.
 */
struct net_buf *bt_hid_alloc_buf(size_t reserve);

/**
 * @brief Common disconnect logic – close Interrupt first, then Control.
 *
 * Sets BT_HID_FLAG_INTR_DISC_PEND and initiates L2CAP disconnect on
 * the Interrupt channel.  The Control channel is closed from the
 * Interrupt channel's disconnected callback.
 *
 * @param conn HID connection.
 */
void bt_hid_conn_close(struct bt_hid_conn *conn);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_BLUETOOTH_HOST_CLASSIC_HID_INTERNAL_H_ */
