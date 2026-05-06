# Bluetooth HID Device Demo (Mouse)

## Overview

This sample implements a Bluetooth Classic (BR/EDR) HID Device that simulates
a 3-button mouse.  After boot, the device automatically becomes discoverable
and connectable — no button press or user interaction is needed.

Once a HID Host connects, the device continuously sends Input Reports at 50 Hz
that move the host cursor in a rectangle (80 × 80 px, 4 px per step).

## Requirements

- A board with a Bluetooth Classic (BR/EDR) capable controller.
  - Tested on: **NXP MIMXRT1170-EVK rev B** (`mimxrt1170_evk@B/mimxrt1176/cm7`)
- A HID Host to connect to (e.g. a PC, or the companion `hid_host` sample
  running on a second board).

## Building and Running

```bash
west build -p -b mimxrt1170_evk@B/mimxrt1176/cm7 samples/bluetooth/classic/hid_device
west flash
```

Open a serial terminal (115200 8N1) to observe log output.

## Expected Output

```
=== Bluetooth HID Device Demo (Mouse) ===
HID Device ready – waiting for host connection
HID Device: connected
HID Device: SET_PROTOCOL -> Report
...
```

After the host connects the device silently sends mouse reports.  If the host
sends `SUSPEND` / `EXIT_SUSPEND` the device stops / resumes sending reports and
logs the event.  On disconnect the device re-enters discoverable mode
automatically.

## How It Works

| Step | Action |
|------|--------|
| 1 | `bt_enable()` initialises the Bluetooth controller |
| 2 | `bt_hid_device_register()` registers the HID Device role and SDP record |
| 3 | `bt_br_set_connectable()` / `bt_br_set_discoverable()` make the device visible |
| 4 | Host connects and the HID `connected` callback fires |
| 5 | A delayable work item sends a 5-byte Input Report every 20 ms |
| 6 | On disconnect the work item is cancelled and discoverable mode is re-entered |

## Configuration

Key Kconfig options (`prj.conf`):

| Option | Value | Purpose |
|--------|-------|---------|
| `CONFIG_BT_DEVICE_NAME` | `"Zephyr HID Mouse"` | Bluetooth device name |
| `CONFIG_BT_COD` | `0x002580` | Class of Device: Peripheral / Mouse |
| `CONFIG_BT_HID_DEVICE_MAX_REPORT_DESC_LEN` | `128` | Max HID descriptor length |
