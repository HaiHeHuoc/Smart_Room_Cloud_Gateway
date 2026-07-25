# AGENTS.md — ESP32-S3 Smart Room Cloud Gateway

## Project Role
You are assisting with an ESP-IDF embedded project for ESP32-S3.
The goal is to build a practical IoT device with:

- BLE Wi-Fi provisioning
- Wi-Fi station mode
- LVGL LCD dashboard
- Sensor monitoring
- Firebase Realtime Database logging
- NVS configuration storage
- Button factory reset

This is a learning-oriented embedded project. Do not over-engineer or implement features outside the requested sprint.

---

## Coding Principles

1. Prefer ESP-IDF style C code unless explicitly requested otherwise.
2. Keep changes minimal and scoped to the current task.
3. Do not rewrite unrelated files.
4. Do not add Wi-Fi, BLE, Firebase, MQTT, OTA, or web server code unless the task explicitly asks for it.
5. Use component-based structure.
6. Add clear `ESP_LOGI/W/E` logs for init, state changes, and errors.
7. Always check and return `esp_err_t` where appropriate.
8. Avoid blocking forever inside component APIs unless explicitly designed as a task loop.
9. Avoid calling LVGL APIs from random tasks. UI updates should go through a UI manager/task or a clearly controlled function.
10. Keep memory usage reasonable. Avoid large static buffers unless necessary.

---

## Preferred Project Structure

```text
components/
├── cloud/
│   ├── cloud_manager/
│   └── firebase_auth/
├── connectivity/
│   └── wifi_manager/
├── display/
│   ├── display_driver/
│   └── waveshare__esp_lcd_st7735/
├── sensing/
│   ├── sensor_manager/
│   └── sensor_DHT22/
├── storage/
│   ├── config_manager/
│   └── sd_card_manager/
├── system/
│   ├── common/
│   └── performance_monitor/
└── ui/
    ├── app_gui/
    ├── ui_manager_lvgl/
    ├── lvgl_image_handler/
    └── lvgl_sd_fs/
```

The first-level folders are organizational domains. Their children remain
independent ESP-IDF components with separate public APIs. Only create
components required by the current sprint.

---

## Current MVP Scope

MVP target:

```text
BLE Wi-Fi Provisioning
+ Wi-Fi Station
+ LVGL LCD Dashboard
+ Sensor Monitor
+ Firebase Realtime Database Logging
+ NVS Config Storage
+ Button Factory Reset
```

Out of scope for early sprints:

```text
Custom mobile app
Firestore direct integration
OTA
MQTT
WebSocket dashboard
BLE always-on data streaming
Complex animation/UI theme
```

---

## Sprint Order

### Sprint 0 — Project Setup
Goal:
- Create clean ESP-IDF project.
- Confirm build/flash/monitor works.
- Add basic component structure only if needed.
- Add clear README skeleton.

Do not implement LVGL/Wi-Fi/BLE/Firebase yet.

### Sprint 1 — LCD + LVGL Bring-up
Goal:
- Initialize LCD display.
- Initialize LVGL.
- Show a simple screen.
- Update one counter/status label periodically.

Out of scope:
- Wi-Fi
- BLE
- Firebase
- Sensor
- NVS

### Sprint 2 — Wi-Fi Hardcoded + LVGL Status
Goal:
- Connect Wi-Fi using hardcoded credentials or menuconfig.
- Display Wi-Fi state/IP/RSSI on LVGL screen.

### Sprint 3 — Sensor + UI
Goal:
- Read sensor periodically.
- Send sensor event to UI.
- Update LVGL labels safely.

### Sprint 4 — Firebase Realtime Database
Goal:
- Upload latest sensor JSON through HTTPS REST.
- Show cloud sync state on LVGL.

### Sprint 5 — NVS + BLE Provisioning
Goal:
- Store Wi-Fi credentials in NVS.
- Use BLE provisioning to receive credentials.
- Connect Wi-Fi after provisioning.
- Stop BLE after successful provisioning.

### Sprint 6 — Factory Reset + Polish
Goal:
- Long press button clears Wi-Fi config.
- Reconnect strategy.
- Error states.
- Documentation and demo preparation.

---

## Task Response Requirements

After making changes, always summarize:

1. Files changed
2. What was implemented
3. What was intentionally not implemented
4. How to build
5. How to flash/monitor
6. Expected serial log/output
7. Risks or follow-up tasks

---

## Build Commands

Use typical ESP-IDF commands:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Do not assume the COM port. Ask the user to provide it or leave `<PORT>` placeholder.

---

## Hardware Assumptions

Target board:
- ESP32-S3, likely N16R3 variant

Potential peripherals:
- ST7735 LCD 128x160 over SPI
- DHT22 or similar temperature/humidity sensor
- Button for factory reset
- LED or RGB LED for status

Do not assume exact pins unless the user provides them.

---

## Quality Bar

The code should be:

- Buildable
- Small enough to review
- Easy to debug
- Properly logged
- Suitable for learning
- Not overly clever

If a requested implementation depends on missing hardware pins, ESP-IDF version, or library choice, clearly state the assumptions and keep the implementation easy to adjust.
