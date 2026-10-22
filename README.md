# ESPHome.RemodoX

ESPHome external component that receives key events from a **Remotec Remodo X** Bluetooth LE remote through an ESP32 and exposes them to Home Assistant (for example to switch the garden light).

Derived from [fsievers22/esphome-ble-remote](https://github.com/fsievers22/esphome-ble-remote) (a generic BLE HID client). This version is **dedicated to Remodo X**: the keepalive, wake-up and reconnect logic follows the behaviour of the Remodo X firmware (RTL8762C, version `20211006`), not generic HID remotes.

> Status: experimental. The Remodo-specific logic comes from static analysis of the firmware and has not yet been verified over a long run on real hardware.

## 1. Program the remote in the Remodo app (once)
The remote must already send the right keys. The component only receives them, it does not program the remote. Use the **REMODO** app (Android / iOS), version 1.4 or newer:

1. Put the remote in **Setup Mode**: hold **Key 3 + Key 4** until you hear a beep. The LED blinks **red**. The app can only connect in this mode.
2. In the app add the controller. When asked for the platform choose **Windows PC** (the app says to use it for single-board computers like a Raspberry Pi, and an ESP32 is treated the same way).
3. If the app says the firmware is outdated, run the OTA update first (needed: `20211006`).
4. Open **Key Mapping Profiles** and create a profile. For each key choose the type **HID keys** → **Keyboard** and assign:

   | Remodo X key | Single Press | Long Press |
   |---|---|---|
   | Key 1 | **A** | **E** |
   | Key 2 | **B** | **F** |
   | Key 3 | **C** | **G** |
   | Key 4 | **D** | **H** |

   (Letters are keyboard usages 4 to 11, so they arrive as `7_4` … `7_11`, exactly what `example.yaml` expects. You can use other keys, then adjust the mapping in the YAML.)
5. Tap **Apply This Profile** so the mapping is written to the remote.
6. Switch the remote to **Control Mode**: hold **Key 1 + Key 2** until you hear a beep. The LED blinks **green**. All HID keys work only in this mode.
7. Make sure the remote is not paired with another computer or phone. The remote remembers the **first** device it pairs with in Control Mode. To forget it, hold **Key 1 + Key 4** until you hear a beep (and remove `Remodo_X_xxxxx` from the Bluetooth list of the other device).

LED: **red** = Setup Mode, **green** = Control Mode.

## 2. Flash the ESP32
Copy [`example.yaml`](example.yaml), set `remodo_mac` in the `substitutions:` block at the top to the address of your remote (the remote advertises as `Remodo_X_xxxxx`, but that suffix is not the end of the address, so take the address from the ESPHome log line `Found device ...`) and flash it. When the remote is in Control Mode and not bonded, the ESP32 connects and pairs with it automatically. The log should show `Remodo connected` and `[configure] keepalive …`.

Minimal configuration:
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
    type: last_event_code
    id: remodo_last_code
    internal: true
```
Entities:
- `last_event_value`: `1` on press, `0` on release (when the remote reports it).
- `last_event_code`: usage page and usage as `page_usage`, e.g. `7_4` for keyboard **A**.
- `last_event_usage`: readable name, e.g. `KEYBOARD_A`.
- With the native API enabled an `esphome.hid_events` event is also fired with `usage` and `value`.

`example.yaml` turns the eight keys into one `event` entity (`button_1_press` … `button_4_long_press`) that you can use in Home Assistant automations. `remodox_id` is only needed on `sensor` / `text_sensor` when you use more than one `remodox:`.

## Two or more remotes
Up to three remotes can run on one ESP32. Give each its own `ble_client` and its own `remodox:` entry, and name the `remodox_id` on every `sensor` / `text_sensor`. All state is kept per remote, log lines are prefixed with the remote's address, and the `esphome.hid_events` event carries an `address` field. See [`example-two-remotes.yaml`](example-two-remotes.yaml). Every extra connection takes airtime from Wi-Fi, so the link margin is smaller than with one remote.

## Diagnostics
[`example-debug.yaml`](example-debug.yaml) is `example.yaml` with everything needed to analyse problems: component log levels, a log of the remote's advertisements (`Remodo_X_…` awake, `Find_Remodo_X_…` asleep), connection state, Wi-Fi signal, free heap and the reset reason. The component logs, per remote:
- every connect and disconnect with the decoded HCI reason, the connection length and the time since the last key,
- pairing result, connection parameter changes, service list, CCCD (notification) writes,
- every notification and decoded key, with timing,
- every keepalive / wake write,
- a `[stats]` line every 60 s (state, counters, seconds since the last key, free heap).

## What the component does for Remodo X
Facts from analysing firmware `20211006`:
- The remote disconnects itself after **5 minutes without activity** (HCI reason `0x13`), resets, and wakes in a sleep mode that has **no HID service** and advertises as `Find_Remodo_X_…`. A host that stays connected to it makes the remote ignore key presses.
- A short key press is sent when the key is **released**, and only if the host has enabled notifications at that moment. The press that wakes a sleeping remote is therefore usually lost, so the link should stay up.
- A write of **Alert Level `0`** (Immediate Alert `0x1802` / `0x2A06`) restarts the remote's idle timer. In sleep mode it restores the previous mode and restarts the remote (wake-up without a key press).

So the component:
1. writes Alert Level `0` every 90 s while connected (keepalive, the remote does not fall asleep),
2. writes Alert Level `0` once when it connects to a remote without a HID service (wake),
3. additionally keeps the older HID Control Point keepalive (Exit Suspend / Suspend),
4. raises the connection supervision timeout to at least 8 s (the remote asks for 3 s, which is fragile with WiFi coexistence).
5. drops a "connected but dead" link: if encryption fails (stale bond, `ESP_AUTH_SMP_ENC_FAIL`) or the notification (CCCD) write is rejected, it removes the stale bond, disconnects and lets the remote pair again. Without this the link stays up, the remote believes it is connected and ignores key presses.

## Credits and licence
Original component by [@fsievers22](https://github.com/fsievers22/esphome-ble-remote). See [`LICENSE`](LICENSE).
