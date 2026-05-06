/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth HID Device Role implementation (HID Profile v1.1.2).
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
#include <zephyr/bluetooth/classic/sdp.h>
#include <zephyr/bluetooth/classic/hid.h>

#include "hid_internal.h"

#define LOG_MODULE_NAME bt_hid_device
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, CONFIG_BT_HID_LOG_LEVEL);

/* -------------------------------------------------------------------------
 * Connection pool
 * -------------------------------------------------------------------------
 */

static struct bt_hid_conn hid_dev_pool[CONFIG_BT_HID_DEVICE_MAX_CONN];
static const struct bt_hid_device_cb *hid_dev_cb;
static const struct bt_hid_device_sdp_config *hid_dev_config;

/* -------------------------------------------------------------------------
 * SDP record
 *
 * Static scalar attribute values – filled at registration time.
 * -------------------------------------------------------------------------
 */

/* Scalar attribute storage (BE where needed) */
static uint16_t sdp_parser_ver_be;
static uint8_t  sdp_device_subclass;
static uint8_t  sdp_country_code;
static uint8_t  sdp_virtual_cable;
static uint8_t  sdp_reconnect_initiate;
static uint8_t  sdp_boot_device;
static uint8_t  sdp_battery_power;
static uint8_t  sdp_remote_wake;
static uint16_t sdp_supervision_timeout_be;
static uint8_t  sdp_normally_connectable;
static uint16_t sdp_ssr_max_latency_be;
static uint16_t sdp_ssr_min_timeout_be;

/*
 * HIDDescriptorList (attribute 0x0206) – dynamic because the report
 * descriptor is supplied at registration time.
 *
 * Wire layout (HID Spec §5.3.4.7):
 *   SEQ8 outer {
 *     SEQ8 inner { UINT8(0x22), TEXT_STR8(report_descriptor) }
 *   }
 *
 * The SDP walker (copy_attribute in sdp.c) casts .data of a SEQ element
 * to (const struct bt_sdp_data_elem *) and iterates via pointer arithmetic,
 * decrementing a counter by sub_elem->total_size each step.  Therefore each
 * SEQ's .data_size must equal the SUM of .total_size of its children in
 * SDP wire-format bytes (NOT sizeof of the C array).
 */
#define HID_DESC_MAX_LEN  CONFIG_BT_HID_DEVICE_MAX_REPORT_DESC_LEN

static uint8_t hid_desc_buf[HID_DESC_MAX_LEN];
static const uint8_t hid_desc_type_byte = 0x22u;  /* ClassDescriptorType = Report */

static struct bt_sdp_data_elem hid_desc_inner[2];   /* [UINT8(0x22), TEXT_STR8]          */
static struct bt_sdp_data_elem hid_desc_list_entry; /* inner SEQ8 wrapping hid_desc_inner */
static struct bt_sdp_data_elem hid_desc_outer_elem; /* outer SEQ8 wrapping list_entry     */

/*
 * Maximum number of SDP attributes in the HID record.
 * Mandatory: 18, optional battery/wake/timeout/connectable/ssr = up to 6 more.
 */
#define HID_SDP_ATTR_MAX  24u

static struct bt_sdp_attribute hid_dev_attrs[HID_SDP_ATTR_MAX];
static struct bt_sdp_record    hid_dev_record;

/* -------------------------------------------------------------------------
 * SDP record builder
 *
 * We follow the same pattern as other profiles in this directory
 * (avrcp.c, rfcomm.c, did.c …): a static struct bt_sdp_attribute[] with
 * BT_SDP_NEW_SERVICE / BT_SDP_LIST / BT_SDP_DATA_ELEM_LIST macros for the
 * compile-time-known attributes, and manual sdp_attr_set() calls for the
 * small number of attributes whose values are only known at registration time.
 * -------------------------------------------------------------------------
 */

/* Compile-time-static portion of the record. */
static struct bt_sdp_attribute hid_static_attrs[] = {
	BT_SDP_NEW_SERVICE,

	/* ServiceClassIDList */
	BT_SDP_LIST(
		BT_SDP_ATTR_SVCLASS_ID_LIST,
		BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
		BT_SDP_DATA_ELEM_LIST(
		{
			BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
			BT_SDP_ARRAY_16(BT_SDP_HID_SVCLASS)
		},
		)
	),

	/* ProtocolDescriptorList – Control channel PSM 0x0011
	 *   SEQ8(13) {
	 *     SEQ8(6) { UUID16(L2CAP), UINT16(0x0011) }
	 *     SEQ8(3) { UUID16(HID)  }
	 *   }
	 */
	BT_SDP_LIST(
		BT_SDP_ATTR_PROTO_DESC_LIST,
		BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
		BT_SDP_DATA_ELEM_LIST(
		{
			BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
			BT_SDP_DATA_ELEM_LIST(
			{
				BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
				BT_SDP_ARRAY_16(BT_SDP_PROTO_L2CAP)
			},
			{
				BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
				BT_SDP_ARRAY_16(BT_HID_PSM_CTRL)
			},
			)
		},
		{
			BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
			BT_SDP_DATA_ELEM_LIST(
			{
				BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
				BT_SDP_ARRAY_16(BT_SDP_PROTO_HID)
			},
			)
		},
		)
	),

	/* AdditionalProtocolDescriptorList – Interrupt channel PSM 0x0013
	 *   SEQ8(15) {
	 *     SEQ8(13) {
	 *       SEQ8(6) { UUID16(L2CAP), UINT16(0x0013) }
	 *       SEQ8(3) { UUID16(HID)  }
	 *     }
	 *   }
	 */
	BT_SDP_LIST(
		BT_SDP_ATTR_ADD_PROTO_DESC_LIST,
		BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 15),
		BT_SDP_DATA_ELEM_LIST(
		{
			BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
			BT_SDP_DATA_ELEM_LIST(
			{
				BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
				BT_SDP_DATA_ELEM_LIST(
				{
					BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
					BT_SDP_ARRAY_16(BT_SDP_PROTO_L2CAP)
				},
				{
					BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
					BT_SDP_ARRAY_16(BT_HID_PSM_INTR)
				},
				)
			},
			{
				BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
				BT_SDP_DATA_ELEM_LIST(
				{
					BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
					BT_SDP_ARRAY_16(BT_SDP_PROTO_HID)
				},
				)
			},
			)
		},
		)
	),

	/* BluetoothProfileDescriptorList
	 *   SEQ8(8) { SEQ8(6) { UUID16(HID_SVCLASS), UINT16(0x0101) } }
	 */
	BT_SDP_LIST(
		BT_SDP_ATTR_PROFILE_DESC_LIST,
		BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 8),
		BT_SDP_DATA_ELEM_LIST(
		{
			BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
			BT_SDP_DATA_ELEM_LIST(
			{
				BT_SDP_TYPE_SIZE(BT_SDP_UUID16),
				BT_SDP_ARRAY_16(BT_SDP_HID_SVCLASS)
			},
			{
				BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
				BT_SDP_ARRAY_16(0x0101)   /* HID Profile v1.1 */
			},
			)
		},
		)
	),

	/* HIDLANGIDBaseList (0x0207)
	 *   SEQ8(8) { SEQ8(6) { UINT16(0x0409), UINT16(0x0100) } }
	 */
	BT_SDP_LIST(
		BT_SDP_ATTR_HID_LANG_ID_BASE_LIST,
		BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 8),
		BT_SDP_DATA_ELEM_LIST(
		{
			BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
			BT_SDP_DATA_ELEM_LIST(
			{
				BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
				BT_SDP_ARRAY_16(0x0409)   /* English (US) */
			},
			{
				BT_SDP_TYPE_SIZE(BT_SDP_UINT16),
				BT_SDP_ARRAY_16(0x0100)   /* base offset  */
			},
			)
		},
		)
	),
};

#define HID_STATIC_ATTR_COUNT  ARRAY_SIZE(hid_static_attrs)

static void sdp_attr_set(struct bt_sdp_attribute *a,
			 uint16_t id, uint8_t type,
			 uint32_t data_size, uint32_t total_size,
			 const void *data)
{
	a->id             = id;
	a->val.type       = type;
	a->val.data_size  = data_size;
	a->val.total_size = total_size;
	a->val.data       = data;
}

static int build_sdp_record(const struct bt_hid_device_sdp_config *cfg)
{
	uint8_t n = 0;
	uint32_t inner_data_size;

	if (cfg->report_desc_len > HID_DESC_MAX_LEN) {
		LOG_ERR("Report descriptor too large (%u > %u)",
			cfg->report_desc_len, HID_DESC_MAX_LEN);
		return -ENOMEM;
	}

	/* Copy static attributes */
	memcpy(hid_dev_attrs, hid_static_attrs,
	       HID_STATIC_ATTR_COUNT * sizeof(struct bt_sdp_attribute));
	n = HID_STATIC_ATTR_COUNT;

	/* Copy report descriptor into static buffer */
	memcpy(hid_desc_buf, cfg->report_desc, cfg->report_desc_len);

	/*
	 * Build HIDDescriptorList element tree (Spec §5.3.4.7):
	 *   SEQ8 outer { SEQ8 inner { UINT8(0x22), TEXT_STR8(desc) } }
	 *
	 * leaf wire sizes: UINT8 = 2, TEXT_STR8(N) = N+2
	 */
	hid_desc_inner[0].type       = BT_SDP_UINT8;
	hid_desc_inner[0].data_size  = 1u;
	hid_desc_inner[0].total_size = 2u;
	hid_desc_inner[0].data       = &hid_desc_type_byte;

	hid_desc_inner[1].type       = BT_SDP_TEXT_STR8;
	hid_desc_inner[1].data_size  = cfg->report_desc_len;
	hid_desc_inner[1].total_size = cfg->report_desc_len + 2u;
	hid_desc_inner[1].data       = hid_desc_buf;

	inner_data_size = hid_desc_inner[0].total_size + hid_desc_inner[1].total_size;

	hid_desc_list_entry.type       = BT_SDP_SEQ8;
	hid_desc_list_entry.data_size  = inner_data_size;
	hid_desc_list_entry.total_size = inner_data_size + 2u;
	hid_desc_list_entry.data       = hid_desc_inner;

	hid_desc_outer_elem.type       = BT_SDP_SEQ8;
	hid_desc_outer_elem.data_size  = hid_desc_list_entry.total_size;
	hid_desc_outer_elem.total_size = hid_desc_list_entry.total_size + 2u;
	hid_desc_outer_elem.data       = &hid_desc_list_entry;

	/* Fill scalar BE values */
	sdp_parser_ver_be        = sys_cpu_to_be16(cfg->parser_version);
	sdp_device_subclass      = cfg->device_subclass;
	sdp_country_code         = cfg->country_code;
	sdp_virtual_cable        = cfg->virtual_cable        ? 1u : 0u;
	sdp_reconnect_initiate   = cfg->reconnect_initiate   ? 1u : 0u;
	sdp_boot_device          = cfg->boot_device          ? 1u : 0u;
	sdp_battery_power        = cfg->battery_power        ? 1u : 0u;
	sdp_remote_wake          = cfg->remote_wake          ? 1u : 0u;
	sdp_normally_connectable = cfg->normally_connectable ? 1u : 0u;
	sdp_supervision_timeout_be = sys_cpu_to_be16(cfg->supervision_timeout);
	sdp_ssr_max_latency_be   = sys_cpu_to_be16(cfg->ssr_host_max_latency);
	sdp_ssr_min_timeout_be   = sys_cpu_to_be16(cfg->ssr_host_min_timeout);

	/* Dynamic scalar attributes appended after the static block */

	/* HIDParserVersion (0x0201) */
	sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_PARSER_VERSION,
		     BT_SDP_UINT16, 2u, 3u, &sdp_parser_ver_be);

	/* HIDDeviceSubclass (0x0202) */
	sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_DEVICE_SUBCLASS,
		     BT_SDP_UINT8, 1u, 2u, &sdp_device_subclass);

	/* HIDCountryCode (0x0203) */
	sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_COUNTRY_CODE,
		     BT_SDP_UINT8, 1u, 2u, &sdp_country_code);

	/* HIDVirtualCable (0x0204) */
	sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_VIRTUAL_CABLE,
		     BT_SDP_BOOL, 1u, 2u, &sdp_virtual_cable);

	/* HIDReconnectInitiate (0x0205) */
	sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_RECONNECT_INITIATE,
		     BT_SDP_BOOL, 1u, 2u, &sdp_reconnect_initiate);

	/* HIDDescriptorList (0x0206) – dynamic */
	sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_DESCRIPTOR_LIST,
		     hid_desc_outer_elem.type,
		     hid_desc_outer_elem.data_size,
		     hid_desc_outer_elem.total_size,
		     hid_desc_outer_elem.data);

	/* HIDBootDevice (0x020E) */
	sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_BOOT_DEVICE,
		     BT_SDP_BOOL, 1u, 2u, &sdp_boot_device);

	/* Optional attributes */
	if (cfg->battery_power) {
		sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_BATTERY_POWER,
			     BT_SDP_BOOL, 1u, 2u, &sdp_battery_power);
	}
	if (cfg->remote_wake) {
		sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_REMOTE_WAKEUP,
			     BT_SDP_BOOL, 1u, 2u, &sdp_remote_wake);
	}
	if (cfg->supervision_timeout != 0u) {
		sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_SUPERVISION_TIMEOUT,
			     BT_SDP_UINT16, 2u, 3u, &sdp_supervision_timeout_be);
	}
	if (cfg->normally_connectable) {
		sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_NORMALLY_CONNECTABLE,
			     BT_SDP_BOOL, 1u, 2u, &sdp_normally_connectable);
	}
	if (cfg->ssr_host_max_latency != 0u) {
		sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_SSR_HOST_MAX_LATENCY,
			     BT_SDP_UINT16, 2u, 3u, &sdp_ssr_max_latency_be);
	}
	if (cfg->ssr_host_min_timeout != 0u) {
		sdp_attr_set(&hid_dev_attrs[n++], BT_SDP_ATTR_HID_SSR_HOST_MIN_TIMEOUT,
			     BT_SDP_UINT16, 2u, 3u, &sdp_ssr_min_timeout_be);
	}

	hid_dev_record.attrs      = hid_dev_attrs;
	hid_dev_record.attr_count = n;
	return 0;
}

/* -------------------------------------------------------------------------
 * Connection pool management
 * -------------------------------------------------------------------------
 */

static struct bt_hid_conn *hid_dev_conn_new(struct bt_conn *acl)
{
	for (int i = 0; i < ARRAY_SIZE(hid_dev_pool); i++) {
		struct bt_hid_conn *c = &hid_dev_pool[i];

		if (c->acl == NULL) {
			memset(c, 0, sizeof(*c));
			c->acl      = bt_conn_ref(acl);
			c->state    = BT_HID_DISCONNECTED;
			c->protocol = BT_HID_PROTO_REPORT_MODE;
			atomic_clear(&c->flags);
			k_sem_init(&c->ctrl_tx_sem, 1, 1);
			return c;
		}
	}
	return NULL;
}

static void hid_dev_conn_free(struct bt_hid_conn *conn)
{
	if (conn->acl) {
		bt_conn_unref(conn->acl);
		conn->acl = NULL;
	}
	conn->state = BT_HID_DISCONNECTED;
}

static struct bt_hid_conn *hid_dev_conn_by_acl(struct bt_conn *acl)
{
	for (int i = 0; i < ARRAY_SIZE(hid_dev_pool); i++) {
		if (hid_dev_pool[i].acl == acl) {
			return &hid_dev_pool[i];
		}
	}
	return NULL;
}

/* -------------------------------------------------------------------------
 * HIDP message send helpers
 * -------------------------------------------------------------------------
 */

static int hid_dev_ctrl_send_data(struct bt_hid_conn *conn, uint8_t rtype,
				  const uint8_t *data, uint16_t len)
{
	struct net_buf *buf;
	int err;

	if ((uint32_t)len + 1u > conn->ctrl_chan.tx.mtu) {
		return -EMSGSIZE;
	}
	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_add_u8(buf, bt_hidp_hdr(BT_HIDP_DATA, rtype & 0x03u));
	net_buf_add_mem(buf, data, len);
	err = bt_l2cap_chan_send(&conn->ctrl_chan.chan, buf);
	if (err < 0) {
		net_buf_unref(buf);
		return -EIO;
	}
	return 0;
}

static int hid_dev_intr_send_data(struct bt_hid_conn *conn,
				  const uint8_t *data, uint16_t len)
{
	struct net_buf *buf;
	int err;

	if ((uint32_t)len + 1u > conn->intr_chan.tx.mtu) {
		return -EMSGSIZE;
	}
	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return -ENOMEM;
	}
	net_buf_add_u8(buf, bt_hidp_hdr(BT_HIDP_DATA, BT_HID_REPORT_TYPE_INPUT));
	net_buf_add_mem(buf, data, len);
	err = bt_l2cap_chan_send(&conn->intr_chan.chan, buf);
	if (err < 0) {
		net_buf_unref(buf);
		return -EIO;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * HIDP message handlers
 * -------------------------------------------------------------------------
 */

static void hid_dev_handle_get_report(struct bt_hid_conn *conn,
				      uint8_t hdr, struct net_buf *buf)
{
	uint8_t  rtype    = bt_hidp_param(hdr) & 0x03u;
	bool     has_size = (bt_hidp_param(hdr) & 0x08u) != 0u;
	uint8_t  rid      = 0u;
	uint16_t bsize    = 0u;

	if (buf->len > 0u) {
		rid = net_buf_pull_u8(buf);
	}
	if (has_size && buf->len >= 2u) {
		bsize = net_buf_pull_le16(buf);
	}
	if (rtype == BT_HID_REPORT_TYPE_RESERVED) {
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_INVALID_PARAM);
		return;
	}
	if (hid_dev_cb && hid_dev_cb->get_report) {
		hid_dev_cb->get_report(conn, rtype, rid, bsize);
	} else {
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_UNSUPPORTED);
	}
}

static void hid_dev_handle_set_report(struct bt_hid_conn *conn,
				      uint8_t hdr, struct net_buf *buf)
{
	uint8_t rtype = bt_hidp_param(hdr) & 0x03u;

	if (rtype == BT_HID_REPORT_TYPE_RESERVED) {
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_INVALID_PARAM);
		return;
	}
	if (hid_dev_cb && hid_dev_cb->set_report) {
		hid_dev_cb->set_report(conn, rtype, buf->data, buf->len);
	} else {
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_UNSUPPORTED);
	}
}

static void hid_dev_handle_get_protocol(struct bt_hid_conn *conn)
{
	struct net_buf *buf;
	int err;

	if (hid_dev_config && !hid_dev_config->boot_device) {
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_UNSUPPORTED);
		return;
	}
	if (hid_dev_cb && hid_dev_cb->get_protocol) {
		hid_dev_cb->get_protocol(conn);
		return;
	}
	buf = bt_hid_alloc_buf(0u);
	if (!buf) {
		return;
	}
	net_buf_add_u8(buf, bt_hidp_hdr(BT_HIDP_DATA, BT_HID_REPORT_TYPE_OTHER));
	net_buf_add_u8(buf, conn->protocol);
	err = bt_l2cap_chan_send(&conn->ctrl_chan.chan, buf);
	if (err < 0) {
		net_buf_unref(buf);
	}
}

static void hid_dev_handle_set_protocol(struct bt_hid_conn *conn, uint8_t hdr)
{
	uint8_t proto = bt_hidp_param(hdr) & 0x01u;

	if (hid_dev_config && !hid_dev_config->boot_device) {
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_UNSUPPORTED);
		return;
	}
	conn->protocol = proto;
	if (hid_dev_cb && hid_dev_cb->set_protocol) {
		hid_dev_cb->set_protocol(conn, proto);
		return;
	}
	bt_hid_device_send_handshake(conn, BT_HIDP_HANDSHAKE_SUCCESSFUL);
}

static void hid_dev_handle_hid_control(struct bt_hid_conn *conn, uint8_t hdr)
{
	uint8_t op = bt_hidp_param(hdr);

	switch (op) {
	case BT_HIDP_CTRL_SUSPEND:
		conn->state = BT_HID_SUSPENDED;
		if (hid_dev_cb && hid_dev_cb->suspend) {
			hid_dev_cb->suspend(conn);
		}
		break;
	case BT_HIDP_CTRL_EXIT_SUSPEND:
		if (conn->state == BT_HID_SUSPENDED) {
			conn->state = BT_HID_CONNECTED;
		}
		if (hid_dev_cb && hid_dev_cb->exit_suspend) {
			hid_dev_cb->exit_suspend(conn);
		}
		break;
	case BT_HIDP_CTRL_VIRTUAL_CABLE_UNPLUG:
		atomic_set_bit(&conn->flags, BT_HID_FLAG_VC_UNPLUG);
		if (hid_dev_cb && hid_dev_cb->virtual_cable_unplug) {
			hid_dev_cb->virtual_cable_unplug(conn);
		}
		bt_hid_conn_close(conn);
		break;
	default:
		break;
	}
}

/* -------------------------------------------------------------------------
 * L2CAP channel ops - Control
 * -------------------------------------------------------------------------
 */

static void hid_dev_ctrl_connected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, ctrl_chan.chan);

	atomic_set_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED);
	conn->state = BT_HID_INTR_CONNECTING;
	/*
	 * The Device role is the L2CAP server for both PSM 0x0011 and 0x0013.
	 * Do NOT call bt_l2cap_chan_connect() here – the Host initiates the
	 * Interrupt channel connection after the Control channel is up
	 * (HID Spec §5.2.2).  The device simply waits for the incoming
	 * L2CAP connect request on PSM 0x0013 via hid_dev_intr_accept().
	 */
}

static void hid_dev_ctrl_disconnected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, ctrl_chan.chan);

	atomic_clear_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED);
	if (conn->state == BT_HID_CTRL_DISCONNECTING ||
	    conn->state == BT_HID_CONNECTED ||
	    conn->state == BT_HID_SUSPENDED) {
		if (hid_dev_cb && hid_dev_cb->disconnected) {
			hid_dev_cb->disconnected(conn, conn->disconnect_reason);
		}
		hid_dev_conn_free(conn);
	}
}

static int hid_dev_ctrl_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, ctrl_chan.chan);
	uint8_t hdr, msg_type;

	if (buf->len < 1u) {
		return 0;
	}
	hdr      = net_buf_pull_u8(buf);
	msg_type = bt_hidp_msg_type(hdr);

	switch (msg_type) {
	case BT_HIDP_GET_REPORT:
		hid_dev_handle_get_report(conn, hdr, buf);
		break;
	case BT_HIDP_SET_REPORT:
		hid_dev_handle_set_report(conn, hdr, buf);
		break;
	case BT_HIDP_GET_PROTOCOL:
		hid_dev_handle_get_protocol(conn);
		break;
	case BT_HIDP_SET_PROTOCOL:
		hid_dev_handle_set_protocol(conn, hdr);
		break;
	case BT_HIDP_HID_CONTROL:
		hid_dev_handle_hid_control(conn, hdr);
		break;
	case BT_HIDP_GET_IDLE:
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_UNSUPPORTED);
		break;
	case BT_HIDP_SET_IDLE:
		if (buf->len >= 1u && buf->data[0] == 0u) {
			bt_hid_device_send_handshake(conn,
						     BT_HIDP_HANDSHAKE_SUCCESSFUL);
		} else {
			bt_hid_device_send_handshake(conn,
						     BT_HIDP_HANDSHAKE_ERR_INVALID_PARAM);
		}
		break;
	default:
		bt_hid_device_send_handshake(conn,
					     BT_HIDP_HANDSHAKE_ERR_UNSUPPORTED);
		break;
	}
	return 0;
}

static const struct bt_l2cap_chan_ops hid_dev_ctrl_ops = {
	.connected    = hid_dev_ctrl_connected,
	.disconnected = hid_dev_ctrl_disconnected,
	.recv         = hid_dev_ctrl_recv,
};

/* -------------------------------------------------------------------------
 * L2CAP channel ops - Interrupt
 * -------------------------------------------------------------------------
 */

static void hid_dev_intr_connected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, intr_chan.chan);

	atomic_set_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED);
	conn->state = BT_HID_CONNECTED;
	if (hid_dev_cb && hid_dev_cb->connected) {
		hid_dev_cb->connected(conn);
	}
}

static void hid_dev_intr_disconnected(struct bt_l2cap_chan *chan)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, intr_chan.chan);

	atomic_clear_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED);
	if (atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		conn->state = BT_HID_CTRL_DISCONNECTING;
		bt_l2cap_chan_disconnect(&conn->ctrl_chan.chan);
	} else {
		if (hid_dev_cb && hid_dev_cb->disconnected) {
			hid_dev_cb->disconnected(conn, conn->disconnect_reason);
		}
		hid_dev_conn_free(conn);
	}
}

static int hid_dev_intr_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	struct bt_hid_conn *conn =
		CONTAINER_OF(chan, struct bt_hid_conn, intr_chan.chan);
	uint8_t hdr;
	uint8_t mtype;

	if (buf->len < 1u) {
		return 0;
	}
	hdr   = net_buf_pull_u8(buf);
	mtype = bt_hidp_msg_type(hdr);

	if (mtype != BT_HIDP_DATA) {
		return 0;
	}
	if (hid_dev_cb && hid_dev_cb->output_report) {
		hid_dev_cb->output_report(conn, buf->data, buf->len);
	}
	return 0;
}

static const struct bt_l2cap_chan_ops hid_dev_intr_ops = {
	.connected    = hid_dev_intr_connected,
	.disconnected = hid_dev_intr_disconnected,
	.recv         = hid_dev_intr_recv,
};

/* -------------------------------------------------------------------------
 * L2CAP server accept callbacks
 * -------------------------------------------------------------------------
 */

static int hid_dev_ctrl_accept(struct bt_conn *acl,
			       struct bt_l2cap_server *server,
			       struct bt_l2cap_chan **chan)
{
	struct bt_hid_conn *conn = hid_dev_conn_by_acl(acl);

	if (!conn) {
		conn = hid_dev_conn_new(acl);
		if (!conn) {
			return -ENOMEM;
		}
	}
	conn->ctrl_chan.chan.ops = &hid_dev_ctrl_ops;
	conn->ctrl_chan.rx.mtu  = BT_HID_MTU_REPORT;
	conn->intr_chan.chan.ops = &hid_dev_intr_ops;
	conn->intr_chan.rx.mtu  = BT_HID_MTU_REPORT;
	conn->state = BT_HID_CTRL_CONNECTING;
	*chan = &conn->ctrl_chan.chan;
	return 0;
}

static int hid_dev_intr_accept(struct bt_conn *acl,
			       struct bt_l2cap_server *server,
			       struct bt_l2cap_chan **chan)
{
	struct bt_hid_conn *conn = hid_dev_conn_by_acl(acl);

	if (!conn) {
		return -ENOENT;
	}
	*chan = &conn->intr_chan.chan;
	return 0;
}

static struct bt_l2cap_server hid_dev_ctrl_server = {
	.psm       = BT_HID_PSM_CTRL,
	.sec_level = BT_SECURITY_L2,
	.accept    = hid_dev_ctrl_accept,
};

static struct bt_l2cap_server hid_dev_intr_server = {
	.psm       = BT_HID_PSM_INTR,
	.sec_level = BT_SECURITY_L2,
	.accept    = hid_dev_intr_accept,
};

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------
 */

int bt_hid_device_register(const struct bt_hid_device_sdp_config *config,
			   const struct bt_hid_device_cb *cb)
{
	int err;

	if (!config || !config->report_desc || config->report_desc_len == 0u) {
		return -EINVAL;
	}
	if (hid_dev_cb != NULL) {
		return -EALREADY;
	}
	err = build_sdp_record(config);
	if (err) {
		return err;
	}
	err = bt_sdp_register_service(&hid_dev_record);
	if (err) {
		LOG_ERR("SDP register failed: %d", err);
		return err;
	}
	err = bt_l2cap_br_server_register(&hid_dev_ctrl_server);
	if (err) {
		LOG_ERR("ctrl L2CAP register failed: %d", err);
		return err;
	}
	err = bt_l2cap_br_server_register(&hid_dev_intr_server);
	if (err) {
		LOG_ERR("intr L2CAP register failed: %d", err);
		return err;
	}
	hid_dev_config = config;
	hid_dev_cb     = cb;
	LOG_INF("HID Device registered (subclass=0x%02x)", config->device_subclass);
	return 0;
}

int bt_hid_device_send_input_report(struct bt_hid_conn *conn,
				    const uint8_t *data, uint16_t len)
{
	if (!conn || !data) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_INTR_CONNECTED)) {
		return -ENOTCONN;
	}
	return hid_dev_intr_send_data(conn, data, len);
}

int bt_hid_device_send_report(struct bt_hid_conn *conn, uint8_t type,
			      const uint8_t *data, uint16_t len)
{
	if (!conn || !data) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}
	return hid_dev_ctrl_send_data(conn, type, data, len);
}

int bt_hid_device_send_handshake(struct bt_hid_conn *conn, uint8_t result_code)
{
	if (!conn) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}
	return bt_hid_send_ctrl_byte(conn,
		bt_hidp_hdr(BT_HIDP_HANDSHAKE, result_code & 0x0Fu));
}

int bt_hid_device_virtual_cable_unplug(struct bt_hid_conn *conn)
{
	int err;

	if (!conn) {
		return -EINVAL;
	}
	if (!atomic_test_bit(&conn->flags, BT_HID_FLAG_CTRL_CONNECTED)) {
		return -ENOTCONN;
	}
	atomic_set_bit(&conn->flags, BT_HID_FLAG_VC_UNPLUG);
	err = bt_hid_send_ctrl_byte(conn,
		bt_hidp_hdr(BT_HIDP_HID_CONTROL,
			    BT_HIDP_CTRL_VIRTUAL_CABLE_UNPLUG));
	if (err) {
		return err;
	}
	if (hid_dev_cb && hid_dev_cb->virtual_cable_unplug) {
		hid_dev_cb->virtual_cable_unplug(conn);
	}
	bt_hid_conn_close(conn);
	return 0;
}

int bt_hid_device_disconnect(struct bt_hid_conn *conn)
{
	if (!conn) {
		return -EINVAL;
	}
	if (conn->state == BT_HID_DISCONNECTED) {
		return -ENOTCONN;
	}
	bt_hid_conn_close(conn);
	return 0;
}
