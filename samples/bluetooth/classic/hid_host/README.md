# Bluetooth HID Host Demo

## Overview

This sample implements a Bluetooth Classic (BR/EDR) HID Host.  After boot it
automatically starts a BR/EDR inquiry scan (~10 s) and connects to the first
device whose Class of Device indicates a Peripheral (HID Device).  No button
press or user interaction is required.

Once the HID connection is established the host:

- Reads key SDP HID attributes (parser version, device subclass, boot device, virtual cable).
- Sends `GET_PROTOCOL` to query the device's current protocol mode.
- Receives and prints all Input reports arriving on the Interrupt channel.
- Sends `GET_REPORT(Input, ID=1)` on the Control channel every 5 seconds.
- Reconnects automatically 5 s after a disconnection.

## Requirements

- A board with a Bluetooth Classic (BR/EDR) capable controller.
  - Tested on: **NXP MIMXRT1170-EVK rev B** (`mimxrt1170_evk@B/mimxrt1176/cm7`)
- A HID Device to connect to (e.g. a Bluetooth mouse, keyboard, or the
  companion `hid_device` sample running on a second board).

## Building and Running

```bash
west build -p -b mimxrt1170_evk@B/mimxrt1176/cm7 samples/bluetooth/classic/hid_host
west flash
```

Open a serial terminal (115200 8N1) to observe log output.

## Expected Output

```
=== Bluetooth HID Host Demo ===
HID Host ready – starting inquiry scan
Inquiry started (~10 s)...
Found: XX:XX:XX:XX:XX:XX CoD=0x002580
  -> HID Device, connecting...
ACL connected: XX:XX:XX:XX:XX:XX
Security level 2 established
HID Host: HID connection established
HID Host: SDP record found
  HIDParserVersion:  0x0111
  HIDDeviceSubclass: 0x80
  HIDBootDevice:     TRUE
  HIDVirtualCable:   TRUE
HID Host: Protocol mode = Report
HID Host: INPUT type=1 len=5  [ 01 00 02 fe 00 ]
  Mouse btn=0 X=  +2 Y=  -2 wheel=  0
...
HID Host: GET_REPORT rsp type=1 len=5  [ 01 00 00 00 00 ]
```

On disconnect the host waits 5 s and then reconnects to the same device
automatically (address stored in `hid_dev_addr`).

## How It Works

| Step | Action |
|------|--------|
| 1 | `bt_enable()` initialises the Bluetooth controller |
| 2 | `bt_hid_host_register()` registers the HID Host role |
| 3 | `bt_br_discovery_start()` starts a 10 s inquiry scan |
| 4 | After the scan window a work item calls `process_discovery_results()` |
| 5 | The first Peripheral CoD device is connected via `bt_conn_create_br()` |
| 6 | Security (pairing) is established; then `bt_hid_host_connect()` opens HID channels |
| 7 | SDP attributes are read and `GET_PROTOCOL` is issued |
| 8 | Input reports are printed; `GET_REPORT` fires every 5 s |
| 9 | On HID disconnect, `reconnect_work` fires after 5 s |

## Configuration

Key Kconfig options (`prj.conf`):

| Option | Value | Purpose |
|--------|-------|---------|
| `CONFIG_BT_DEVICE_NAME` | `"Zephyr HID Host"` | Bluetooth device name |
| `CONFIG_BT_MAX_CONN` | `2` | ACL + HID connection slots |
| `CONFIG_BT_HID_HOST_MAX_CONN` | `2` | Max simultaneous HID connections |
| `CONFIG_BT_HID_TX_BUF_SIZE` | `672` | Transmit buffer (large enough for SDP) |
