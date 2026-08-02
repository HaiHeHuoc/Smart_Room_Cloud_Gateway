# ESP32-S3 Smart Room Cloud Gateway — Version 1 Release

**Release:** `v1.0.0`  
**Release date:** 2026-08-02  
**Status:** Complete / hardware accepted  
**Owner:** Trần Long Hải (`HaiHeHuoc888`)

## Release Statement

Version 1 covers Sprints 0–9 of the ESP32-S3 Smart Room Cloud Gateway roadmap.
The owner confirmed completion of the remaining target-hardware acceptance
items on 2026-08-02. This document is the authoritative Version 1 release
record; the long roadmap remains implementation history.

## Release Scope

```text
ESP32-S3 N16R8
+ ST7735 LCD and LVGL dashboard
+ microSD and LVGL filesystem
+ Wi-Fi Station management and reconnect
+ DHT22 sensor monitoring
+ Firebase Authentication and Realtime Database telemetry
+ NVS configuration validation, persistence, migration, and reset
+ BLE Security 1 Wi-Fi provisioning with LCD QR code
+ GPIO9 factory reset and reboot-to-provisioning recovery
+ bounded cloud retry and latest-value telemetry
+ CPU, heap, PSRAM, DMA, task, and stack diagnostics
+ architecture, setup, demo, limitations, and security documentation
```

## Sprint Status

| Sprint | Scope | Status |
|---:|---|---|
| 0 | Project setup | Complete |
| 1 | LCD + LVGL | Complete |
| 2 | Wi-Fi Station + GUI | Complete |
| 3 | Sensor manager + UI | Complete |
| 4 | Firebase telemetry | Complete |
| 5 | NVS configuration | Complete |
| 6 | BLE Wi-Fi provisioning | Complete |
| 7 | Factory reset + recovery | Complete |
| 8 | Reconnect + cloud retry | Complete |
| 9 | Product documentation and media | Complete |

## Accepted Behaviors

- [x] Clean ESP-IDF component structure and boot flow.
- [x] ST7735/LVGL rendering and GUI ownership.
- [x] Wi-Fi connect, disconnect, IPv4, and reconnect behavior.
- [x] DHT22 sampling, stale/error state, and GUI updates.
- [x] Firebase sign-in, token refresh, HTTPS upload, and cloud state.
- [x] NVS save, load, migration, persistence, clear, and reset behavior.
- [x] BLE provisioning, QR flow, credential handoff, retry, and cleanup.
- [x] Factory-reset qualification, ordered cleanup, UI result, and reboot.
- [x] Network/cloud recovery and newest-value retention.
- [x] Runtime stability and resource observations.
- [x] Product README, setup, architecture, security, demo, and limitations.

## Product Evidence

### Video

[Watch the Version 1 hardware demo](https://youtube.com/shorts/9C5_hecEgXA?feature=share)

### Images

| Evidence | File |
|---|---|
| Wired prototype | `docs/media/hardware-overview.jpg` |
| BLE provisioning | `docs/media/screen-provisioning.jpg` |
| Wi-Fi status | `docs/media/screen-wifi.jpg` |
| Sensor dashboard | `docs/media/screen-dashboard.jpg` |
| Factory-reset result | `docs/media/screen-reset-result.jpg` |

See [`docs/media/README.md`](docs/media/README.md) for the rendered gallery and
sanitization rules.

## Security Boundary

Version 1 is suitable as a portfolio/development release. It is not a
production device-security baseline.

Current protections include:

- TLS certificate verification;
- Firebase Email/Password Authentication;
- ID-token refresh and optional exact UID validation;
- Realtime Database authorization through Security Rules;
- Git-ignored local `sdkconfig` values;
- no intentional credential or token logging;
- sensitive temporary-buffer zeroization.

Current limitations include:

- the device password is compiled into development firmware;
- no hardware-backed identity, NVS encryption, flash encryption, or secure boot;
- no signed OTA or fleet credential-rotation policy;
- App Check is not implemented.

Before public publication, complete the owner actions in [`SECURITY.md`](SECURITY.md),
especially credential rotation and Git-history review. Removing a credential
from the current tree does not invalidate older commits.

## Version 2 Architecture Baseline

Version 2 must preserve the Version 1 ownership boundaries:

- `wifi_manager`: Wi-Fi Station lifecycle and reconnect;
- `provisioning_manager`: temporary BLE provisioning;
- `config_manager`: persistent application configuration;
- `cloud_manager`: telemetry and retry;
- `firebase_auth`: authentication and token lifecycle;
- `app_gui` / `ui_manager_lvgl`: UI models, screens, and LVGL;
- `button_manager`: input events;
- `app_reset_coordinator`: factory-reset orchestration;
- future `audio_manager`: I2S, DMA, microphone, speaker, and PCM buffering;
- future `voice_assistant`: isolation of `esp_xiaozhi` from the application.

## Release Boundary

```text
VERSION 1 — COMPLETE
NEXT — VERSION 2 / AUDIO AND XIAOZHI EXTENSION
```
