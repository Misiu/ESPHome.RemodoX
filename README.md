# ESPHome.RemodoX

ESPHome external component that receives key events from a **Remotec Remodo X** Bluetooth LE remote through an ESP32 and exposes them to Home Assistant.

Derived from [fsievers22/esphome-ble-remote](https://github.com/fsievers22/esphome-ble-remote) (generic BLE HID client). This fork is **dedicated to Remodo X**: the keepalive, wake-up and reconnect logic follows the behaviour of the Remodo X firmware (RTL8762C, `20211006`), not generic HID remotes.

> Status: experimental. The Remodo-specific parts are based on static analysis of the firmware and have not yet been verified on hardware.

## Requirements
- ESP32 with the `esp-idf` framework (developed on ESP32-S3). Upstream notes that boards without PSRAM seem not to be compatible.
- The remote's keys are programmed **once in the Remodo app** (Control Mode, platform "Windows PC", keys mapped to keyboard usages). This component only receives the events.

## Usage
```yaml
external_components:
  - source: github://Misiu/ESPHome.RemodoX@main
    components: [remodox]

esp32_ble:
  io_capability: keyboard_display

esp32_ble_tracker:

ble_client:
  - id: remodo_ble
    mac_address: "AA:BB:CC:DD:EE:FF"
    auto_connect: true

remodox:
  id: remodo_x
  ble_client_id: remodo_ble

sensor:
  - platform: remodox
    type: last_event_value
    id: remodo_last_value
    internal: true

text_sensor:
  - platform: remodox
    type: last_event_code        # "<usage page>_<usage>", e.g. "7_4" = keyboard A
    internal: true
```
See [`example.yaml`](example.yaml). The optional `remodox_id` key on `sensor` / `text_sensor` is only needed with more than one `remodox:`.

Entities:
- `last_event_value`: `1` on press, `0` on release (when the remote reports it).
- `last_event_code`: usage page and usage as `page_usage`.
- `last_event_usage`: human readable usage name, e.g. `KEYBOARD_A`.
- With the native API enabled, an `esphome.hid_events` event is also fired with `usage` and `value`.

## What the component does for Remodo X
Facts below come from analysing firmware `20211006`:
- The remote disconnects itself after **5 minutes without activity** (HCI reason `0x13`), resets, and wakes in a sleep mode that has **no HID service** and advertises as `Find_Remodo_X_…`. A host that stays connected to it makes the remote ignore key presses.
- Key presses are only sent after the host has enabled notifications, and a short press is sent on key release.
- Writing **Alert Level `0`** (Immediate Alert `0x1802` / `0x2A06`) restarts the remote's idle timer, and wakes it when it sleeps.

So the component:
1. writes Alert Level `0` every 90 s while connected (keepalive, so the remote does not fall asleep),
2. writes Alert Level `0` once when it connects to a remote without a HID service (wake),
3. additionally keeps the older HID Control Point keepalive (Exit Suspend / Suspend),
4. raises the connection supervision timeout to at least 8 s (the remote asks for 3 s, which is fragile with WiFi coexistence).

## Credits and licence
Original component by [@fsievers22](https://github.com/fsievers22/esphome-ble-remote). See [`LICENSE`](LICENSE).
