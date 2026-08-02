# ESP32-S3 Smart Room Cloud Gateway — Version 1 Release

**Release:** `v1.0.0`  
**Release date:** 2026-08-02  
**Status:** Complete  
**Owner:** Trần Long Hải (`HaiHeHuoc888`)

## Release Statement

Version 1 covers Sprints 0–9 of the ESP32-S3 Smart Room Cloud Gateway roadmap.

On 2026-08-02, the owner explicitly confirmed that all remaining test and target-hardware acceptance items are complete and requested that no additional verification or feature work be added before closing Version 1.

This release record is the authoritative Version 1 closure status. Earlier roadmap sections may preserve historical pending checkboxes from the implementation period; those entries are superseded for Version 1 release status by this owner-confirmed closure record.

## Version 1 Scope

```text
ESP32-S3 N16R8
+ ST7735 LCD and LVGL dashboard
+ SD card and LVGL filesystem support
+ Wi-Fi Station management
+ DHT22 sensor monitoring
+ Firebase Authentication and Realtime Database telemetry
+ NVS configuration storage and migration
+ BLE Security 1 Wi-Fi provisioning with LCD QR code
+ Button-driven factory reset and reboot-to-provisioning recovery
+ Wi-Fi reconnect and bounded cloud retry
+ Runtime CPU, heap, PSRAM, DMA, task, and stack diagnostics
+ Portfolio architecture, setup, demo, limitations, and release documentation
```

## Sprint Closure

| Sprint | Scope | Version 1 status |
|---:|---|---|
| 0 | Project setup | Complete |
| 1 | LCD + LVGL bring-up | Complete |
| 2 | Wi-Fi Station + LVGL status | Complete |
| 3 | Sensor manager + UI | Complete |
| 4 | Firebase Realtime Database upload | Complete |
| 5 | NVS configuration storage | Complete |
| 6 | BLE Wi-Fi provisioning | Complete |
| 7 | Button factory reset + recovery | Complete |
| 8 | Reconnect + cloud retry | Complete |
| 9 | Portfolio polish | Complete |

## Final Acceptance Record

The following Version 1 acceptance areas are marked complete by explicit owner confirmation:

- [x] Clean ESP-IDF project structure and boot flow.
- [x] ST7735 LCD and LVGL rendering.
- [x] Queue-driven GUI ownership and screen routing.
- [x] Wi-Fi Station connect, disconnect, IPv4, and reconnect behavior.
- [x] DHT22 periodic sampling, stale/error behavior, and GUI updates.
- [x] Firebase authentication, HTTPS telemetry upload, and status display.
- [x] NVS save, load, migration, persistence, invalid-state handling, clear, and reset behavior.
- [x] BLE provisioning service, QR flow, credentials handoff, retry, timeout, cleanup, and Wi-Fi adoption.
- [x] Provisioning replacement-session, stale-generation, retry-exhaustion, and recovery behavior.
- [x] Factory-reset button debounce, long press, ordered reset transaction, UI result, persistent cleanup, and reboot-to-provisioning flow.
- [x] Reset behavior during relevant Wi-Fi and provisioning lifecycle states.
- [x] Wi-Fi outage, Internet outage, cloud backoff, transport/HTTP/auth recovery, and latest-value telemetry behavior.
- [x] Runtime stability, watchdog/crash checks, task ownership, and resource observations.
- [x] README, architecture, setup, demo, security, limitations, and future-roadmap documentation.

No additional production feature is required to close Version 1.

## Architecture Baseline For Version 2

Version 2 must preserve the existing Version 1 ownership boundaries:

- `wifi_manager` owns Wi-Fi Station lifecycle and reconnect.
- `provisioning_manager` owns temporary BLE provisioning.
- `config_manager` owns persistent application configuration.
- `cloud_manager` owns Firebase telemetry and retry.
- `firebase_auth` owns token lifecycle.
- `app_gui` owns application screens, copied models, queues, and LVGL objects.
- `ui_manager_lvgl` owns LVGL initialization, display integration, tick, and synchronization.
- `button_manager` publishes input events only.
- `app_reset_coordinator` owns factory-reset orchestration.
- Future `audio_manager` must own microphone, speaker, I2S, DMA, and PCM buffering.
- Future `voice_assistant` must isolate `esp_xiaozhi` from the rest of the application.

## Optional Portfolio Media

Media is an optional presentation improvement and is not a Version 1 release blocker.

```text
HaiHeHuoc888: update here + add the real prototype overview photo
HaiHeHuoc888: update here + add the provisioning QR screen photo with sensitive QR content blurred
HaiHeHuoc888: update here + add the Wi-Fi status screen photo
HaiHeHuoc888: update here + add the sensor dashboard photo
HaiHeHuoc888: update here + add the factory-reset result screen photo
HaiHeHuoc888: update here + add a sanitized Firebase latest-value screenshot
HaiHeHuoc888: update here + add a performance-monitor screenshot
HaiHeHuoc888: update here + add the final Version 1 demo video link
```

Detailed filenames and sanitization requirements are documented in `docs/media/V1_MEDIA_PLACEHOLDERS.md`.

## Security Notes Before Public Publication

- Rotate credentials that previously appeared in Git history.
- Keep Firebase development values in local `sdkconfig` through menuconfig.
- Never publish Wi-Fi passwords, Firebase passwords, tokens, provisioning PoP values, QR payloads, private database URLs, or personal browser/account information.
- Treat firmware binaries and serial logs as potentially sensitive development artifacts.

## Release Boundary

Version 1 is closed. New audio and Xiaozhi work begins in Version 2 and must follow the existing post-V1 sprint order rather than replacing or reopening Sprints 0–9.

```text
VERSION 1 — COMPLETE
NEXT BASELINE — VERSION 2 / AUDIO AND XIAOZHI EXTENSION
```