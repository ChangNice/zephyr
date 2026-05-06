/*
 * Copyright (c) 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth HID Host demo.
 *
 * After Bluetooth initialisation the host automatically starts a BR/EDR
 * inquiry scan (~10 s).  The first discovered device whose Class of Device
 * indicates a Peripheral (HID Device) is connected automatically.
 *
 * Once the HID connection is established the host:
 *   - Reads key SDP HID attributes.
 *   - Sends GET_PROTOCOL to query the device's protocol mode.
 *   - Receives and prints all Input reports from the Interrupt channel.
 *   - Sends GET_REPORT(Input, ID=1) every 5 seconds.
 *   - Reconnects automatically 5 s after a disconnection.
 *
 * No user interaction is required.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/classic/sdp.h>
#include <zephyr/bluetooth/classic/hid.h>

/* -------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------
 */
static struct bt_hid_conn *hid_conn;
static bt_addr_t           hid_dev_addr;
static bool                hid_dev_addr_valid;

static struct k_work_delayable get_report_work;
#define GET_REPORT_INTERVAL_MS  5000

static struct k_work_delayable reconnect_work;
#define RECONNECT_DELAY_MS      5000

/* -------------------------------------------------------------------------
 * SDP discovery
 * -------------------------------------------------------------------------
 */
NET_BUF_POOL_FIXED_DEFINE(hid_sdp_pool, 1,
			  BT_L2CAP_SDU_BUF_SIZE(512),
			  CONFIG_BT_CONN_TX_USER_DATA_SIZE,
			  NULL);

static uint8_t hid_sdp_cb(struct bt_conn *conn,
			  struct bt_sdp_client_result *result,
			  const struct bt_sdp_discover_params *params);

static struct bt_sdp_discover_params hid_sdp_params = {
	.func = hid_sdp_cb,
	.pool = &hid_sdp_pool,
	.type = BT_SDP_DISCOVER_SERVICE_SEARCH_ATTR,
};

static struct bt_uuid_16 hid_svc_uuid = BT_UUID_INIT_16(BT_SDP_HID_SVCLASS);

static uint8_t hid_sdp_cb(struct bt_conn *conn,
			  struct bt_sdp_client_result *result,
			  const struct bt_sdp_discover_params *params)
{
	struct bt_sdp_attribute attr;
	int err;

	if (!result) {
		printk("HID Host: SDP discovery complete\n");
		return BT_SDP_DISCOVER_UUID_STOP;
	}

	if (!result->resp_buf) {
		return BT_SDP_DISCOVER_UUID_CONTINUE;
	}

	printk("HID Host: SDP record found\n");

	err = bt_sdp_get_attr(result->resp_buf, BT_SDP_ATTR_HID_PARSER_VERSION, &attr);
	if (err == 0 && attr.val.type == BT_SDP_UINT16) {
		uint16_t ver = sys_be16_to_cpu(*(uint16_t *)attr.val.data);

		printk("  HIDParserVersion:  0x%04x\n", ver);
	}

	err = bt_sdp_get_attr(result->resp_buf, BT_SDP_ATTR_HID_DEVICE_SUBCLASS, &attr);
	if (err == 0) {
		uint8_t sub = *(uint8_t *)attr.val.data;

		printk("  HIDDeviceSubclass: 0x%02x\n", sub);
	}

	err = bt_sdp_get_attr(result->resp_buf, BT_SDP_ATTR_HID_BOOT_DEVICE, &attr);
	if (err == 0) {
		printk("  HIDBootDevice:     %s\n",
		       (*(uint8_t *)attr.val.data) ? "TRUE" : "FALSE");
	}

	err = bt_sdp_get_attr(result->resp_buf, BT_SDP_ATTR_HID_VIRTUAL_CABLE, &attr);
	if (err == 0) {
		printk("  HIDVirtualCable:   %s\n",
		       (*(uint8_t *)attr.val.data) ? "TRUE" : "FALSE");
	}

	return result->next_record_hint ? BT_SDP_DISCOVER_UUID_CONTINUE
					: BT_SDP_DISCOVER_UUID_STOP;
}

static void start_sdp_discovery(struct bt_conn *conn)
{
	hid_sdp_params.uuid = &hid_svc_uuid.uuid;
	int err = bt_sdp_discover(conn, &hid_sdp_params);

	if (err) {
		printk("SDP discover failed: %d\n", err);
	}
}

/* -------------------------------------------------------------------------
 * Periodic GET_REPORT
 * -------------------------------------------------------------------------
 */
static void get_report_handler(struct k_work *work)
{
	if (!hid_conn) {
		return;
	}

	int err = bt_hid_host_get_report(hid_conn, BT_HID_REPORT_TYPE_INPUT, 0x01, 0);

	if (err && err != -EBUSY) {
		printk("GET_REPORT failed: %d\n", err);
	}

	k_work_reschedule(&get_report_work, K_MSEC(GET_REPORT_INTERVAL_MS));
}

/* -------------------------------------------------------------------------
 * Reconnect
 * -------------------------------------------------------------------------
 */
static void reconnect_handler(struct k_work *work)
{
	struct bt_conn *acl;

	if (hid_conn || !hid_dev_addr_valid) {
		return;
	}

	printk("HID Host: reconnecting...\n");
	acl = bt_conn_create_br(&hid_dev_addr, BT_BR_CONN_PARAM_DEFAULT);
	if (!acl) {
		printk("  bt_conn_create_br failed, retry in %d s\n",
		       RECONNECT_DELAY_MS / 1000);
		k_work_reschedule(&reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
		return;
	}
	bt_conn_unref(acl);
}

/* -------------------------------------------------------------------------
 * ACL connection callbacks
 * -------------------------------------------------------------------------
 */
static void acl_connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_STR_LEN];
	struct bt_conn_info info;

	if (err) {
		printk("ACL connect failed: 0x%02x\n", err);
		return;
	}

	bt_conn_get_info(conn, &info);
	bt_addr_to_str(info.br.dst, addr, sizeof(addr));
	printk("ACL connected: %s\n", addr);

	int sec_err = bt_conn_set_security(conn, BT_SECURITY_L2);

	if (sec_err && sec_err != -EALREADY) {
		printk("bt_conn_set_security failed: %d\n", sec_err);
	}
}

static void acl_disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_STR_LEN];
	struct bt_conn_info info;

	bt_conn_get_info(conn, &info);
	bt_addr_to_str(info.br.dst, addr, sizeof(addr));
	printk("ACL disconnected: %s (reason 0x%02x)\n", addr, reason);
}

static void acl_security_changed(struct bt_conn *conn, bt_security_t level,
				 enum bt_security_err sec_err)
{
	struct bt_conn_info info;

	bt_conn_get_info(conn, &info);
	if (info.type != BT_CONN_TYPE_BR) {
		return;
	}

	if (sec_err) {
		char addr[BT_ADDR_STR_LEN];

		bt_addr_to_str(info.br.dst, addr, sizeof(addr));
		printk("Security failed: %s err %d\n", addr, sec_err);
		return;
	}

	printk("Security level %d established\n", level);

	if (!hid_conn) {
		hid_conn = bt_hid_host_connect(conn);
		if (!hid_conn) {
			printk("bt_hid_host_connect failed\n");
		}
	}
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected        = acl_connected,
	.disconnected     = acl_disconnected,
	.security_changed = acl_security_changed,
};

/* -------------------------------------------------------------------------
 * HID Host callbacks
 * -------------------------------------------------------------------------
 */
static void hid_host_connected(struct bt_hid_conn *conn)
{
	struct bt_conn *acl = bt_hid_conn_get_acl(conn);

	printk("HID Host: HID connection established\n");

	if (acl) {
		start_sdp_discovery(acl);
	}

	int err = bt_hid_host_get_protocol(conn);

	if (err) {
		printk("GET_PROTOCOL failed: %d\n", err);
	}

	k_work_reschedule(&get_report_work, K_MSEC(GET_REPORT_INTERVAL_MS));
}

static void hid_host_disconnected(struct bt_hid_conn *conn, uint8_t reason)
{
	printk("HID Host: disconnected (reason 0x%02x)\n", reason);
	k_work_cancel_delayable(&get_report_work);
	hid_conn = NULL;

	if (hid_dev_addr_valid) {
		printk("HID Host: reconnect in %d s\n", RECONNECT_DELAY_MS / 1000);
		k_work_reschedule(&reconnect_work, K_MSEC(RECONNECT_DELAY_MS));
	}
}

static void hid_host_vc_unplug(struct bt_hid_conn *conn)
{
	printk("HID Host: Virtual Cable Unplug\n");
	hid_dev_addr_valid = false;
	k_work_cancel_delayable(&reconnect_work);
}

static void hid_host_input_report(struct bt_hid_conn *conn, uint8_t type,
				  const uint8_t *data, uint16_t len)
{
	printk("HID Host: INPUT type=%u len=%u  [ ", type, len);
	for (uint16_t i = 0; i < len; i++) {
		printk("%02x ", data[i]);
	}
	printk("]\n");

	/* Decode mouse report: [ID][buttons][X][Y][wheel] */
	if (len >= 5 && data[0] == 0x01) {
		int8_t x = (int8_t)data[2];
		int8_t y = (int8_t)data[3];

		if ((data[1] & 0x07u) || x || y || data[4]) {
			printk("  Mouse btn=%01x X=%+4d Y=%+4d wheel=%+3d\n",
			       data[1] & 0x07u, x, y, (int8_t)data[4]);
		}
	}
}

static void hid_host_get_report_rsp(struct bt_hid_conn *conn, uint8_t type,
				    const uint8_t *data, uint16_t len)
{
	printk("HID Host: GET_REPORT rsp type=%u len=%u  [ ", type, len);
	for (uint16_t i = 0; i < len; i++) {
		printk("%02x ", data[i]);
	}
	printk("]\n");
}

static void hid_host_handshake(struct bt_hid_conn *conn, uint8_t result_code)
{
	static const char * const result_str[] = {
		"SUCCESSFUL", "NOT_READY", "ERR_INVALID_REPORT_ID",
		"ERR_UNSUPPORTED_REQUEST", "ERR_INVALID_PARAMETER",
	};
	const char *s = (result_code < ARRAY_SIZE(result_str))
			? result_str[result_code] : "RESERVED";

	printk("HID Host: HANDSHAKE 0x%02x (%s)\n", result_code, s);
}

static void hid_host_protocol_mode(struct bt_hid_conn *conn, uint8_t mode)
{
	printk("HID Host: Protocol mode = %s\n",
	       mode == BT_HID_PROTO_BOOT_MODE ? "Boot" : "Report");
}

static const struct bt_hid_host_cb hid_host_cb = {
	.connected            = hid_host_connected,
	.disconnected         = hid_host_disconnected,
	.virtual_cable_unplug = hid_host_vc_unplug,
	.input_report         = hid_host_input_report,
	.get_report_rsp       = hid_host_get_report_rsp,
	.handshake            = hid_host_handshake,
	.protocol_mode        = hid_host_protocol_mode,
};

/* -------------------------------------------------------------------------
 * BR/EDR device discovery
 * -------------------------------------------------------------------------
 */
#define COD_MAJOR_PERIPHERAL  0x0500u
#define COD_MAJOR_MASK        0x1F00u
#define DISCOVERY_COUNT       5

static struct bt_br_discovery_result discovery_results[DISCOVERY_COUNT];

static const struct bt_br_discovery_param discovery_params = {
	.length  = 8,      /* 8 × 1.28 s ≈ 10 s */
	.limited = false,
};

static void process_discovery_results(void)
{
	for (int i = 0; i < DISCOVERY_COUNT; i++) {
		struct bt_br_discovery_result *r = &discovery_results[i];
		char addr_str[BT_ADDR_STR_LEN];
		uint32_t cod;
		struct bt_conn *conn;

		if (bt_addr_eq(&r->addr, BT_ADDR_ANY)) {
			break;
		}

		bt_addr_to_str(&r->addr, addr_str, sizeof(addr_str));
		cod = ((uint32_t)r->cod[2] << 16) |
		      ((uint32_t)r->cod[1] << 8)  |
		       (uint32_t)r->cod[0];

		printk("Found: %s CoD=0x%06x\n", addr_str, cod);

		if ((cod & COD_MAJOR_MASK) != COD_MAJOR_PERIPHERAL) {
			continue;
		}

		printk("  -> HID Device, connecting...\n");
		bt_br_discovery_stop();

		bt_addr_copy(&hid_dev_addr, &r->addr);
		hid_dev_addr_valid = true;

		conn = bt_conn_create_br(&r->addr, BT_BR_CONN_PARAM_DEFAULT);
		if (conn) {
			bt_conn_unref(conn);
		} else {
			printk("  bt_conn_create_br failed\n");
		}
		return;
	}

	printk("No HID Device found in scan results\n");
}

static struct k_work_delayable inquiry_check_work;
#define INQUIRY_CHECK_MS  (8 * 1280 + 2000)

static void inquiry_check_handler(struct k_work *work)
{
	process_discovery_results();
}

static void start_inquiry(void)
{
	memset(discovery_results, 0, sizeof(discovery_results));

	int err = bt_br_discovery_start(&discovery_params,
					discovery_results,
					ARRAY_SIZE(discovery_results));
	if (err) {
		printk("bt_br_discovery_start failed: %d\n", err);
	} else {
		printk("Inquiry started (~10 s)...\n");
		k_work_reschedule(&inquiry_check_work, K_MSEC(INQUIRY_CHECK_MS));
	}
}

/* -------------------------------------------------------------------------
 * Bluetooth ready – auto-start inquiry
 * -------------------------------------------------------------------------
 */
static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed: %d\n", err);
		return;
	}

	err = bt_hid_host_register(&hid_host_cb);
	if (err) {
		printk("bt_hid_host_register failed: %d\n", err);
		return;
	}

	printk("HID Host ready – starting inquiry scan\n");
	start_inquiry();
}

int main(void)
{
	printk("=== Bluetooth HID Host Demo ===\n");

	k_work_init_delayable(&get_report_work, get_report_handler);
	k_work_init_delayable(&reconnect_work, reconnect_handler);
	k_work_init_delayable(&inquiry_check_work, inquiry_check_handler);

	int err = bt_enable(bt_ready);

	if (err) {
		printk("bt_enable failed: %d\n", err);
		return -1;
	}

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
