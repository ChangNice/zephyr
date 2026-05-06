/*
 * Copyright (c) 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Bluetooth HID Device demo – simulates a 3-button mouse.
 *
 * After Bluetooth initialisation the device automatically enters
 * General Discoverable + Connectable mode.  Once a HID Host connects
 * it continuously sends XY mouse-movement reports that trace a rectangle
 * at 50 Hz.  No user interaction is required.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/classic/hid.h>

/* -------------------------------------------------------------------------
 * HID Report Descriptor – 3-button mouse with XY relative axes
 * Report ID 1 (Input, 4 bytes):
 *   Byte 0[2:0] = Button 1/2/3 | Byte 0[7:3] = padding
 *   Byte 1 = X (signed 8-bit, relative)
 *   Byte 2 = Y (signed 8-bit, relative)
 *   Byte 3 = Wheel (signed 8-bit, relative)
 * -------------------------------------------------------------------------
 */
static const uint8_t hid_mouse_report_desc[] = {
	0x05, 0x01,  /* Usage Page (Generic Desktop)     */
	0x09, 0x02,  /* Usage (Mouse)                    */
	0xA1, 0x01,  /* Collection (Application)         */
	0x85, 0x01,  /*   Report ID (1)                  */
	0x09, 0x01,  /*   Usage (Pointer)                */
	0xA1, 0x00,  /*   Collection (Physical)          */
	0x05, 0x09,  /*     Usage Page (Button)          */
	0x19, 0x01,  /*     Usage Minimum (1)            */
	0x29, 0x03,  /*     Usage Maximum (3)            */
	0x15, 0x00,  /*     Logical Minimum (0)          */
	0x25, 0x01,  /*     Logical Maximum (1)          */
	0x75, 0x01,  /*     Report Size (1)              */
	0x95, 0x03,  /*     Report Count (3)             */
	0x81, 0x02,  /*     Input (Data,Var,Abs)         */
	0x75, 0x05,  /*     Report Size (5) – padding    */
	0x95, 0x01,  /*     Report Count (1)             */
	0x81, 0x01,  /*     Input (Const,Var,Abs)        */
	0x05, 0x01,  /*     Usage Page (Generic Desktop) */
	0x09, 0x30,  /*     Usage (X)                    */
	0x09, 0x31,  /*     Usage (Y)                    */
	0x09, 0x38,  /*     Usage (Wheel)                */
	0x15, 0x81,  /*     Logical Minimum (-127)       */
	0x25, 0x7F,  /*     Logical Maximum (127)        */
	0x75, 0x08,  /*     Report Size (8)              */
	0x95, 0x03,  /*     Report Count (3)             */
	0x81, 0x06,  /*     Input (Data,Var,Rel)         */
	0xC0,        /*   End Collection (Physical)      */
	0xC0,        /* End Collection (Application)     */
};

#define MOUSE_REPORT_ID   0x01
#define MOUSE_REPORT_LEN  5     /* 1 (Report ID) + 4 (payload) */

#define MOUSE_PERIOD_MS   20    /* 50 Hz */
#define MOUSE_STEP_PX     4     /* pixels per report */
#define MOUSE_SIDE_STEPS  20    /* steps per side → side length = 80 px */

/* Rectangle traversal: 0=right, 1=down, 2=left, 3=up */
static struct bt_hid_conn *hid_conn;
static bool connected;
static bool suspended;
static int  mouse_step;   /* step within current side */
static int  mouse_side;   /* current side (0-3) */

static struct k_work_delayable mouse_work;

/* -------------------------------------------------------------------------
 * Mouse report sender
 * -------------------------------------------------------------------------
 */
static void mouse_report_send(struct k_work *work)
{
	uint8_t report[MOUSE_REPORT_LEN];
	/* dx/dy per side: right, down, left, up */
	static const int8_t dx_tbl[4] = { MOUSE_STEP_PX, 0, -MOUSE_STEP_PX, 0 };
	static const int8_t dy_tbl[4] = { 0, MOUSE_STEP_PX, 0, -MOUSE_STEP_PX };
	int8_t dx, dy;

	if (!connected || suspended || !hid_conn) {
		return;
	}

	dx = dx_tbl[mouse_side];
	dy = dy_tbl[mouse_side];
	if (++mouse_step >= MOUSE_SIDE_STEPS) {
		mouse_step = 0;
		mouse_side = (mouse_side + 1) & 3;
	}

	report[0] = MOUSE_REPORT_ID;
	report[1] = 0x00;
	report[2] = dx;
	report[3] = dy;
	report[4] = 0x00;

	int err = bt_hid_device_send_input_report(hid_conn, report, MOUSE_REPORT_LEN);

	if (err && err != -EMSGSIZE) {
		printk("HID send failed: %d\n", err);
	}

	k_work_reschedule(&mouse_work, K_MSEC(MOUSE_PERIOD_MS));
}

/* -------------------------------------------------------------------------
 * HID Device callbacks
 * -------------------------------------------------------------------------
 */
static void hid_connected(struct bt_hid_conn *conn)
{
	printk("HID Device: connected\n");
	hid_conn   = conn;
	connected  = true;
	suspended  = false;
	mouse_step = 0;
	k_work_reschedule(&mouse_work, K_MSEC(MOUSE_PERIOD_MS));
}

static void hid_disconnected(struct bt_hid_conn *conn, uint8_t reason)
{
	printk("HID Device: disconnected (reason 0x%02x)\n", reason);
	k_work_cancel_delayable(&mouse_work);
	connected = false;
	suspended = false;
	hid_conn  = NULL;

	/* Re-enter discoverable so a new host can connect */
	bt_br_set_connectable(true, NULL);
	bt_br_set_discoverable(true, false);
}

static void hid_virtual_cable_unplug(struct bt_hid_conn *conn)
{
	printk("HID Device: Virtual Cable Unplug\n");
}

static void hid_suspend(struct bt_hid_conn *conn)
{
	printk("HID Device: SUSPEND\n");
	suspended = true;
	k_work_cancel_delayable(&mouse_work);
}

static void hid_exit_suspend(struct bt_hid_conn *conn)
{
	printk("HID Device: EXIT_SUSPEND\n");
	suspended = false;
	k_work_reschedule(&mouse_work, K_MSEC(MOUSE_PERIOD_MS));
}

static void hid_get_report(struct bt_hid_conn *conn, uint8_t type,
			   uint8_t id, uint16_t buf_size)
{
	uint8_t report[MOUSE_REPORT_LEN] = {MOUSE_REPORT_ID, 0, 0, 0, 0};
	int err;

	if (type == BT_HID_REPORT_TYPE_INPUT && id == MOUSE_REPORT_ID) {
		err = bt_hid_device_send_report(conn, type, report, MOUSE_REPORT_LEN);
		if (err) {
			printk("GET_REPORT send failed: %d\n", err);
		}
	} else {
		bt_hid_device_send_handshake(conn, BT_HIDP_HANDSHAKE_ERR_INVALID_REPORT_ID);
	}
}

static void hid_set_report(struct bt_hid_conn *conn, uint8_t type,
			   const uint8_t *data, uint16_t len)
{
	bt_hid_device_send_handshake(conn, BT_HIDP_HANDSHAKE_SUCCESSFUL);
}

static void hid_set_protocol(struct bt_hid_conn *conn, uint8_t protocol)
{
	printk("HID Device: SET_PROTOCOL -> %s\n",
	       protocol == BT_HID_PROTO_BOOT_MODE ? "Boot" : "Report");
}

static void hid_get_protocol(struct bt_hid_conn *conn)
{
}

static void hid_output_report(struct bt_hid_conn *conn,
			      const uint8_t *data, uint16_t len)
{
}

static const struct bt_hid_device_cb hid_cb = {
	.connected            = hid_connected,
	.disconnected         = hid_disconnected,
	.virtual_cable_unplug = hid_virtual_cable_unplug,
	.suspend              = hid_suspend,
	.exit_suspend         = hid_exit_suspend,
	.get_report           = hid_get_report,
	.set_report           = hid_set_report,
	.set_protocol         = hid_set_protocol,
	.get_protocol         = hid_get_protocol,
	.output_report        = hid_output_report,
};

/* -------------------------------------------------------------------------
 * HID SDP configuration
 * -------------------------------------------------------------------------
 */
static const struct bt_hid_device_sdp_config hid_sdp_cfg = {
	.parser_version      = 0x0111,
	.device_subclass     = 0x80,   /* Pointing device */
	.country_code        = 0x00,
	.virtual_cable       = true,
	.reconnect_initiate  = true,
	.boot_device         = true,
	.battery_power       = true,
	.remote_wake         = true,
	.supervision_timeout = 3200,
	.report_desc         = hid_mouse_report_desc,
	.report_desc_len     = sizeof(hid_mouse_report_desc),
};

/* -------------------------------------------------------------------------
 * Bluetooth ready – auto-start discoverable
 * -------------------------------------------------------------------------
 */
static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed: %d\n", err);
		return;
	}

	err = bt_hid_device_register(&hid_sdp_cfg, &hid_cb);
	if (err) {
		printk("bt_hid_device_register failed: %d\n", err);
		return;
	}

	/* Automatically become connectable and discoverable */
	bt_br_set_connectable(true, NULL);
	bt_br_set_discoverable(true, false);

	printk("HID Device ready – waiting for host connection\n");
}

int main(void)
{
	printk("=== Bluetooth HID Device Demo (Mouse) ===\n");

	k_work_init_delayable(&mouse_work, mouse_report_send);

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
