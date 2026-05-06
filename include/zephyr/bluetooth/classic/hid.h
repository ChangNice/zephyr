/** @file
 *  @brief Bluetooth HID Profile public API.
 */

/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_INCLUDE_BLUETOOTH_CLASSIC_HID_H_
#define ZEPHYR_INCLUDE_BLUETOOTH_CLASSIC_HID_H_

/**
 * @file
 * @brief Bluetooth Human Interface Device (HID) Profile
 * @defgroup bt_hid Bluetooth HID Profile
 * @ingroup bluetooth
 * @{
 *
 * This API implements the Bluetooth Human Interface Device Profile v1.1.2
 * (HID Profile Specification, Bluetooth SIG).
 *
 * The profile operates over BR/EDR using two L2CAP channels:
 *   - Control Channel  (PSM 0x0011): commands and feature reports
 *   - Interrupt Channel (PSM 0x0013): low-latency input/output reports
 */

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * PSM values (Bluetooth Assigned Numbers)
 * -------------------------------------------------------------------------
 */

/** HID Control channel PSM */
#define BT_HID_PSM_CTRL  0x0011
/** HID Interrupt channel PSM */
#define BT_HID_PSM_INTR  0x0013

/* -------------------------------------------------------------------------
 * HIDP Message Types (high nibble of 1-octet header, Spec §3.1.1)
 * -------------------------------------------------------------------------
 */

/** HANDSHAKE – acknowledgement from Device (Spec §3.1.2.1) */
#define BT_HIDP_HANDSHAKE     0x00u
/** HID_CONTROL – major state change request (Spec §3.1.2.2) */
#define BT_HIDP_HID_CONTROL   0x10u
/** GET_REPORT – Host requests a report from Device (Spec §3.1.2.3) */
#define BT_HIDP_GET_REPORT    0x40u
/** SET_REPORT – Host sends a report to Device (Spec §3.1.2.4) */
#define BT_HIDP_SET_REPORT    0x50u
/** GET_PROTOCOL – Host reads current protocol mode (Spec §3.1.2.5) */
#define BT_HIDP_GET_PROTOCOL  0x60u
/** SET_PROTOCOL – Host sets protocol mode (Spec §3.1.2.6) */
#define BT_HIDP_SET_PROTOCOL  0x70u
/** GET_IDLE (deprecated, Spec §3.1.2.7) */
#define BT_HIDP_GET_IDLE      0x80u
/** SET_IDLE (deprecated, Spec §3.1.2.8) */
#define BT_HIDP_SET_IDLE      0x90u
/** DATA – report payload on Control or Interrupt channel (Spec §3.1.2.9) */
#define BT_HIDP_DATA          0xA0u

/* -------------------------------------------------------------------------
 * HANDSHAKE result codes (low nibble, Spec Table 3.2)
 * -------------------------------------------------------------------------
 */

/** Successful handshake */
#define BT_HIDP_HANDSHAKE_SUCCESSFUL            0x00u
/** Device not ready – Host may retry */
#define BT_HIDP_HANDSHAKE_NOT_READY             0x01u
/** Invalid Report ID */
#define BT_HIDP_HANDSHAKE_ERR_INVALID_REPORT_ID 0x02u
/** Unsupported request */
#define BT_HIDP_HANDSHAKE_ERR_UNSUPPORTED       0x03u
/** Invalid parameter value */
#define BT_HIDP_HANDSHAKE_ERR_INVALID_PARAM     0x04u
/** Unknown error */
#define BT_HIDP_HANDSHAKE_ERR_UNKNOWN           0x0Eu
/** Fatal error – device restart required */
#define BT_HIDP_HANDSHAKE_ERR_FATAL             0x0Fu

/* -------------------------------------------------------------------------
 * HID_CONTROL operations (low nibble, Spec Table 3.3)
 * -------------------------------------------------------------------------
 */

/** NOP (deprecated) */
#define BT_HIDP_CTRL_NOP                0x00u
/** Hard reset (deprecated) */
#define BT_HIDP_CTRL_HARD_RESET         0x01u
/** Soft reset (deprecated) */
#define BT_HIDP_CTRL_SOFT_RESET         0x02u
/** Enter reduced power mode (Spec §3.1.2.2.2) */
#define BT_HIDP_CTRL_SUSPEND            0x03u
/** Exit reduced power mode (Spec §3.1.2.2.2) */
#define BT_HIDP_CTRL_EXIT_SUSPEND       0x04u
/** Virtual cable unplug (Spec §3.1.2.2.3) */
#define BT_HIDP_CTRL_VIRTUAL_CABLE_UNPLUG 0x05u

/* -------------------------------------------------------------------------
 * Report types (used in GET_REPORT / SET_REPORT / DATA, Spec §3.1.2.3)
 * -------------------------------------------------------------------------
 */

/** Reserved */
#define BT_HID_REPORT_TYPE_RESERVED 0x00u
/** Input report (Device → Host) */
#define BT_HID_REPORT_TYPE_INPUT    0x01u
/** Output report (Host → Device) */
#define BT_HID_REPORT_TYPE_OUTPUT   0x02u
/** Feature report (bidirectional, always via Control channel) */
#define BT_HID_REPORT_TYPE_FEATURE  0x03u
/** DATA other (used for protocol response payloads) */
#define BT_HID_REPORT_TYPE_OTHER    0x00u

/* -------------------------------------------------------------------------
 * Protocol modes (Spec §2.1.2)
 * -------------------------------------------------------------------------
 */

/** Boot Protocol Mode */
#define BT_HID_PROTO_BOOT_MODE    0x00u
/** Report Protocol Mode (default) */
#define BT_HID_PROTO_REPORT_MODE  0x01u

/* -------------------------------------------------------------------------
 * Boot protocol Report IDs (Spec Table 3.15)
 * -------------------------------------------------------------------------
 */

/** Boot keyboard report ID (9 octets total: 1 ID + 8 data) */
#define BT_HID_BOOT_REPORT_ID_KEYBOARD  0x01u
/** Boot mouse report ID (4 octets total: 1 ID + 3 data) */
#define BT_HID_BOOT_REPORT_ID_MOUSE     0x02u

/* -------------------------------------------------------------------------
 * SDP Attribute IDs (Spec §5.3.3, HID-specific)
 * These supplement the generic ones already in sdp.h
 * -------------------------------------------------------------------------
 */

/** HID SSR Host Max Latency (optional, Spec §5.3.4.15) */
#define BT_SDP_ATTR_HID_SSR_HOST_MAX_LATENCY  0x020fu
/** HID SSR Host Min Timeout (optional, Spec §5.3.4.16) */
#define BT_SDP_ATTR_HID_SSR_HOST_MIN_TIMEOUT  0x0210u

/* -------------------------------------------------------------------------
 * Connection state
 * -------------------------------------------------------------------------
 */

/**
 * @brief HID connection state.
 */
enum bt_hid_conn_state {
	/** No connection */
	BT_HID_DISCONNECTED = 0,
	/** Control channel connection in progress */
	BT_HID_CTRL_CONNECTING,
	/** Control channel connected; Interrupt channel connection in progress */
	BT_HID_INTR_CONNECTING,
	/** Both channels connected – HID connection established */
	BT_HID_CONNECTED,
	/** SUSPEND command received/sent */
	BT_HID_SUSPENDED,
	/** Interrupt channel disconnection in progress */
	BT_HID_INTR_DISCONNECTING,
	/** Control channel disconnection in progress (after Interrupt closed) */
	BT_HID_CTRL_DISCONNECTING,
};

/* -------------------------------------------------------------------------
 * Opaque connection handle
 * -------------------------------------------------------------------------
 */

/** @brief Opaque HID connection object. */
struct bt_hid_conn;

/* -------------------------------------------------------------------------
 * HID Device Role API
 * -------------------------------------------------------------------------
 */

/**
 * @brief HID Device SDP configuration.
 *
 * Passed to @ref bt_hid_device_register to describe the device's
 * capabilities and build the SDP service record.
 *
 * Mandatory SDP attributes are populated automatically.  Optional attributes
 * are only included when the corresponding value is non-zero (or TRUE).
 */
struct bt_hid_device_sdp_config {
	/**
	 * Version of the USB HID Specification this device was designed to.
	 * Format: 0xJJMN (JJ = major, M = minor, N = sub-minor).
	 * E.g. 0x0111 for HID Spec 1.11 (Spec §5.3.4.2).
	 */
	uint16_t parser_version;

	/**
	 * 8-bit Minor Device Class value (bits 7-2 of CoD Minor class field).
	 * Bit 7: Pointing device, Bit 6: Keyboard.
	 * E.g. 0x40 keyboard, 0x80 mouse, 0xC0 combo (Spec §5.3.4.3).
	 */
	uint8_t device_subclass;

	/**
	 * Country code (USB HID Spec §6.2.1).  0 = not localized.
	 * (Spec §5.3.4.4)
	 */
	uint8_t country_code;

	/** TRUE if device supports Virtual Cables (Spec §5.3.4.5) */
	bool virtual_cable;

	/**
	 * TRUE if device initiates reconnection after link loss
	 * (Spec §5.3.4.6).  Required when @p boot_device is TRUE.
	 */
	bool reconnect_initiate;

	/**
	 * TRUE if device supports Boot Protocol Mode
	 * (Spec §5.3.4.12).
	 */
	bool boot_device;

	/** TRUE if device is battery-powered (Spec §5.3.4.10) */
	bool battery_power;

	/** TRUE if device can wake the host (Spec §5.3.4.11) */
	bool remote_wake;

	/**
	 * TRUE if device stays in page-scan when no connection is active
	 * (Spec §5.3.4.14).
	 */
	bool normally_connectable;

	/**
	 * Recommended baseband Link Supervision Timeout in slots.
	 * 0 = attribute omitted (Host uses 2-second default).
	 * (Spec §5.3.4.13)
	 */
	uint16_t supervision_timeout;

	/**
	 * HIDSSRHostMaxLatency in baseband slots (optional, Spec §5.3.4.15).
	 * 0 = attribute omitted.
	 */
	uint16_t ssr_host_max_latency;

	/**
	 * HIDSSRHostMinTimeout in baseband slots (optional, Spec §5.3.4.16).
	 * 0 = attribute omitted.
	 */
	uint16_t ssr_host_min_timeout;

	/** Pointer to the HID Report Descriptor byte array */
	const uint8_t *report_desc;

	/** Length of @p report_desc in bytes */
	uint16_t report_desc_len;
};

/**
 * @brief HID Device role callback structure.
 *
 * All callbacks are optional (may be NULL) unless noted.
 */
struct bt_hid_device_cb {
	/**
	 * @brief HID connection established.
	 *
	 * Called when both Control and Interrupt L2CAP channels are open.
	 *
	 * @param conn HID connection handle.
	 */
	void (*connected)(struct bt_hid_conn *conn);

	/**
	 * @brief HID connection terminated.
	 *
	 * @param conn   HID connection handle.
	 * @param reason HCI disconnect reason code.
	 */
	void (*disconnected)(struct bt_hid_conn *conn, uint8_t reason);

	/**
	 * @brief VIRTUAL_CABLE_UNPLUG received from Host.
	 *
	 * The application must destroy all stored bonding and Virtual Cable
	 * information for this Host (Spec §3.1.2.2.3).
	 *
	 * @param conn HID connection handle.
	 */
	void (*virtual_cable_unplug)(struct bt_hid_conn *conn);

	/**
	 * @brief SUSPEND received from Host.
	 *
	 * The device should enter reduced-power mode (Spec §3.1.2.2.2).
	 *
	 * @param conn HID connection handle.
	 */
	void (*suspend)(struct bt_hid_conn *conn);

	/**
	 * @brief EXIT_SUSPEND received from Host.
	 *
	 * The device should return to normal operation.
	 *
	 * @param conn HID connection handle.
	 */
	void (*exit_suspend)(struct bt_hid_conn *conn);

	/**
	 * @brief GET_REPORT received from Host.
	 *
	 * The application must call @ref bt_hid_device_send_report to
	 * respond.  If the payload would exceed the negotiated MTU or an
	 * error occurs, call @ref bt_hid_device_send_handshake instead.
	 *
	 * @param conn     HID connection handle.
	 * @param type     Report type (BT_HID_REPORT_TYPE_*).
	 * @param id       Report ID (0 if no Report IDs in descriptor).
	 * @param buf_size Requested buffer size (0 = return full report).
	 */
	void (*get_report)(struct bt_hid_conn *conn, uint8_t type,
			   uint8_t id, uint16_t buf_size);

	/**
	 * @brief SET_REPORT received from Host.
	 *
	 * The application must call @ref bt_hid_device_send_handshake to
	 * acknowledge.
	 *
	 * @param conn HID connection handle.
	 * @param type Report type (BT_HID_REPORT_TYPE_*).
	 * @param data Report payload (without HIDP header).
	 * @param len  Payload length in bytes.
	 */
	void (*set_report)(struct bt_hid_conn *conn, uint8_t type,
			   const uint8_t *data, uint16_t len);

	/**
	 * @brief SET_PROTOCOL received from Host.
	 *
	 * The stack automatically sends a HANDSHAKE(SUCCESSFUL) unless
	 * this callback returns a non-zero result code via
	 * @ref bt_hid_device_send_handshake.
	 *
	 * @param conn     HID connection handle.
	 * @param protocol BT_HID_PROTO_BOOT_MODE or BT_HID_PROTO_REPORT_MODE.
	 */
	void (*set_protocol)(struct bt_hid_conn *conn, uint8_t protocol);

	/**
	 * @brief GET_PROTOCOL received from Host.
	 *
	 * The stack automatically replies with the current protocol mode
	 * unless the application wants to override via this callback.
	 *
	 * @param conn HID connection handle.
	 */
	void (*get_protocol)(struct bt_hid_conn *conn);

	/**
	 * @brief Asynchronous output report received via Interrupt channel.
	 *
	 * @param conn HID connection handle.
	 * @param data Report payload (including Report ID if applicable).
	 * @param len  Payload length in bytes.
	 */
	void (*output_report)(struct bt_hid_conn *conn,
			      const uint8_t *data, uint16_t len);
};

/**
 * @brief Register the HID Device role.
 *
 * Registers L2CAP servers for PSM 0x0011 and 0x0013, builds and registers
 * the SDP service record, and installs the application callbacks.
 *
 * Must be called once before any HID Device connections are accepted.
 *
 * @param config SDP configuration for this device.
 * @param cb     Application callback structure (stored by reference – must
 *               remain valid for the lifetime of the registration).
 *
 * @retval 0        Success.
 * @retval -EINVAL  Invalid parameter.
 * @retval -EALREADY Already registered.
 * @retval -ENOMEM  Insufficient resources.
 */
int bt_hid_device_register(const struct bt_hid_device_sdp_config *config,
			   const struct bt_hid_device_cb *cb);

/**
 * @brief Send an Input report asynchronously via the Interrupt channel.
 *
 * This is the primary method for a Device to report events to the Host.
 * The HIDP DATA header is prepended by the stack.
 *
 * @param conn HID connection handle.
 * @param data Report payload (including Report ID as first byte when
 *             Report IDs are declared in the descriptor, or in Boot mode).
 * @param len  Payload length in bytes.
 *
 * @retval 0        Success.
 * @retval -ENOTCONN Connection not established.
 * @retval -EMSGSIZE Payload exceeds negotiated Interrupt channel MTU.
 * @retval -ENOMEM  No buffer available.
 */
int bt_hid_device_send_input_report(struct bt_hid_conn *conn,
				    const uint8_t *data, uint16_t len);

/**
 * @brief Respond to a GET_REPORT request via the Control channel.
 *
 * Sends a DATA message on the Control channel.  Must be called from within
 * the @ref bt_hid_device_cb.get_report callback.
 *
 * @param conn HID connection handle.
 * @param type Report type (BT_HID_REPORT_TYPE_*).
 * @param data Report payload.
 * @param len  Payload length in bytes.
 *
 * @retval 0        Success.
 * @retval -ENOTCONN Connection not established.
 * @retval -EMSGSIZE Payload plus header would exceed negotiated Control MTU.
 * @retval -ENOMEM  No buffer available.
 */
int bt_hid_device_send_report(struct bt_hid_conn *conn, uint8_t type,
			      const uint8_t *data, uint16_t len);

/**
 * @brief Send a HANDSHAKE message on the Control channel.
 *
 * Used to acknowledge SET_REPORT / SET_PROTOCOL requests, or to report
 * errors for GET_REPORT / GET_PROTOCOL.
 *
 * @param conn        HID connection handle.
 * @param result_code One of BT_HIDP_HANDSHAKE_* constants.
 *
 * @retval 0       Success.
 * @retval -ENOTCONN Not connected.
 * @retval -ENOMEM No buffer available.
 */
int bt_hid_device_send_handshake(struct bt_hid_conn *conn, uint8_t result_code);

/**
 * @brief Initiate Virtual Cable Unplug from the Device.
 *
 * Sends HID_CONTROL(VIRTUAL_CABLE_UNPLUG) to the Host and then performs
 * the required disconnect sequence:
 *   1. Send VIRTUAL_CABLE_UNPLUG on Control channel.
 *   2. Destroy local bonding / Virtual Cable information.
 *   3. Disconnect Interrupt channel (L2CAP Disconnect Request).
 *   4. Disconnect Control channel after Interrupt is closed.
 *
 * (Spec §3.1.2.2.3, §4.5.2)
 *
 * @param conn HID connection handle.
 *
 * @retval 0       Success.
 * @retval -ENOTCONN Not connected.
 */
int bt_hid_device_virtual_cable_unplug(struct bt_hid_conn *conn);

/**
 * @brief Disconnect a HID Device connection.
 *
 * Closes the Interrupt channel first, then the Control channel, per
 * Spec §5.2.2.
 *
 * @param conn HID connection handle.
 *
 * @retval 0       Success.
 * @retval -ENOTCONN Not connected.
 */
int bt_hid_device_disconnect(struct bt_hid_conn *conn);

/**
 * @brief Get the underlying ACL connection from a HID connection.
 *
 * @param conn HID connection handle.
 *
 * @return Pointer to @ref bt_conn, or NULL if not connected.
 */
struct bt_conn *bt_hid_conn_get_acl(const struct bt_hid_conn *conn);

/**
 * @brief Get the current connection state.
 *
 * @param conn HID connection handle.
 *
 * @return Current @ref bt_hid_conn_state.
 */
enum bt_hid_conn_state bt_hid_conn_get_state(const struct bt_hid_conn *conn);

/* -------------------------------------------------------------------------
 * HID Host Role API
 * -------------------------------------------------------------------------
 */

/**
 * @brief HID Host role callback structure.
 *
 * All callbacks are optional (may be NULL) unless noted.
 */
struct bt_hid_host_cb {
	/**
	 * @brief HID connection established.
	 *
	 * Called when both Control and Interrupt channels are open.
	 *
	 * @param conn HID connection handle.
	 */
	void (*connected)(struct bt_hid_conn *conn);

	/**
	 * @brief HID connection terminated.
	 *
	 * @param conn   HID connection handle.
	 * @param reason HCI disconnect reason code.
	 */
	void (*disconnected)(struct bt_hid_conn *conn, uint8_t reason);

	/**
	 * @brief VIRTUAL_CABLE_UNPLUG received from Device.
	 *
	 * The application must destroy all stored bonding and Virtual Cable
	 * information for this Device (Spec §3.1.2.2.3).
	 *
	 * @param conn HID connection handle.
	 */
	void (*virtual_cable_unplug)(struct bt_hid_conn *conn);

	/**
	 * @brief Asynchronous Input report received via Interrupt channel.
	 *
	 * @param conn HID connection handle.
	 * @param type Report type field from DATA HIDP header.
	 * @param data Report payload (including Report ID when applicable).
	 * @param len  Payload length in bytes.
	 */
	void (*input_report)(struct bt_hid_conn *conn, uint8_t type,
			     const uint8_t *data, uint16_t len);

	/**
	 * @brief GET_REPORT response received from Device.
	 *
	 * Called when the Device sends a DATA reply on the Control channel
	 * in response to a previous @ref bt_hid_host_get_report call.
	 *
	 * @param conn HID connection handle.
	 * @param type Report type.
	 * @param data Report payload.
	 * @param len  Payload length in bytes.
	 */
	void (*get_report_rsp)(struct bt_hid_conn *conn, uint8_t type,
			       const uint8_t *data, uint16_t len);

	/**
	 * @brief HANDSHAKE received from Device.
	 *
	 * Called after a SET_REPORT or SET_PROTOCOL request.
	 *
	 * @param conn        HID connection handle.
	 * @param result_code One of BT_HIDP_HANDSHAKE_* constants.
	 */
	void (*handshake)(struct bt_hid_conn *conn, uint8_t result_code);

	/**
	 * @brief Protocol mode response from Device.
	 *
	 * Called when the Device responds to a GET_PROTOCOL request.
	 *
	 * @param conn HID connection handle.
	 * @param mode BT_HID_PROTO_BOOT_MODE or BT_HID_PROTO_REPORT_MODE.
	 */
	void (*protocol_mode)(struct bt_hid_conn *conn, uint8_t mode);
};

/**
 * @brief Register the HID Host role.
 *
 * Installs the application callbacks for incoming Device connections.
 * Must be called before @ref bt_hid_host_connect.
 *
 * @param cb Application callback structure (stored by reference).
 *
 * @retval 0        Success.
 * @retval -EINVAL  NULL callback pointer.
 * @retval -EALREADY Already registered.
 */
int bt_hid_host_register(const struct bt_hid_host_cb *cb);

/**
 * @brief Initiate a HID connection to a remote HID Device.
 *
 * Opens the Control channel first, then the Interrupt channel once the
 * Control channel is fully configured (Spec §5.2.2).
 *
 * The @ref bt_hid_host_cb.connected callback is invoked on success.
 *
 * @param conn  ACL connection to the remote HID Device.
 *
 * @return Pointer to a new @ref bt_hid_conn, or NULL on error.
 */
struct bt_hid_conn *bt_hid_host_connect(struct bt_conn *conn);

/**
 * @brief Send a GET_REPORT request on the Control channel.
 *
 * The Device's response will be delivered via
 * @ref bt_hid_host_cb.get_report_rsp.
 *
 * @param conn     HID connection handle.
 * @param type     Report type (BT_HID_REPORT_TYPE_*).
 * @param id       Report ID (required in Report Protocol if IDs are declared,
 *                 and always required in Boot Protocol Mode).
 *                 Set to 0 when no Report IDs are used.
 * @param buf_size 0 = no size limit; >0 = BufferSize field included.
 *
 * @retval 0        Success.
 * @retval -ENOTCONN Not connected.
 * @retval -EBUSY   Another Control channel transfer is pending.
 * @retval -ENOMEM  No buffer available.
 */
int bt_hid_host_get_report(struct bt_hid_conn *conn, uint8_t type,
			   uint8_t id, uint16_t buf_size);

/**
 * @brief Send a SET_REPORT request on the Control channel.
 *
 * The Device's acknowledgement is delivered via
 * @ref bt_hid_host_cb.handshake.
 *
 * @param conn HID connection handle.
 * @param type Report type (BT_HID_REPORT_TYPE_OUTPUT or _FEATURE).
 * @param data Report payload (including Report ID when applicable).
 * @param len  Payload length in bytes.
 *
 * @retval 0        Success.
 * @retval -ENOTCONN Not connected.
 * @retval -EBUSY   Another Control channel transfer is pending.
 * @retval -EMSGSIZE Payload would exceed negotiated Control channel MTU.
 * @retval -ENOMEM  No buffer available.
 */
int bt_hid_host_set_report(struct bt_hid_conn *conn, uint8_t type,
			   const uint8_t *data, uint16_t len);

/**
 * @brief Send a GET_PROTOCOL request on the Control channel.
 *
 * The Device's response is delivered via
 * @ref bt_hid_host_cb.protocol_mode.
 *
 * @param conn HID connection handle.
 *
 * @retval 0        Success.
 * @retval -ENOTCONN Not connected.
 * @retval -EBUSY   Another Control channel transfer is pending.
 */
int bt_hid_host_get_protocol(struct bt_hid_conn *conn);

/**
 * @brief Send a SET_PROTOCOL request on the Control channel.
 *
 * The Device's acknowledgement is delivered via
 * @ref bt_hid_host_cb.handshake.
 *
 * @param conn     HID connection handle.
 * @param protocol BT_HID_PROTO_BOOT_MODE or BT_HID_PROTO_REPORT_MODE.
 *
 * @retval 0        Success.
 * @retval -ENOTCONN Not connected.
 * @retval -EBUSY   Another Control channel transfer is pending.
 * @retval -EINVAL  Invalid protocol value.
 */
int bt_hid_host_set_protocol(struct bt_hid_conn *conn, uint8_t protocol);

/**
 * @brief Send an Output report asynchronously via the Interrupt channel.
 *
 * @param conn HID connection handle.
 * @param data Report payload (including Report ID when applicable).
 * @param len  Payload length in bytes.
 *
 * @retval 0        Success.
 * @retval -ENOTCONN Not connected.
 * @retval -EMSGSIZE Payload would exceed negotiated Interrupt channel MTU.
 * @retval -ENOMEM  No buffer available.
 */
int bt_hid_host_send_output_report(struct bt_hid_conn *conn,
				   const uint8_t *data, uint16_t len);

/**
 * @brief Send a HID_CONTROL message on the Control channel.
 *
 * Used to send SUSPEND, EXIT_SUSPEND, or VIRTUAL_CABLE_UNPLUG.
 * Note: HID_CONTROL does not generate a HANDSHAKE response
 * (Spec §3.2.1.3).
 *
 * @param conn      HID connection handle.
 * @param operation One of BT_HIDP_CTRL_* constants.
 *
 * @retval 0       Success.
 * @retval -ENOTCONN Not connected.
 * @retval -EINVAL  Unsupported operation for the Host role.
 * @retval -ENOMEM No buffer available.
 */
int bt_hid_host_send_control(struct bt_hid_conn *conn, uint8_t operation);

/**
 * @brief Disconnect a HID Host connection.
 *
 * Closes the Interrupt channel first, then the Control channel
 * (Spec §5.2.2).
 *
 * @param conn HID connection handle.
 *
 * @retval 0       Success.
 * @retval -ENOTCONN Not connected.
 */
int bt_hid_host_disconnect(struct bt_hid_conn *conn);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* ZEPHYR_INCLUDE_BLUETOOTH_CLASSIC_HID_H_ */
