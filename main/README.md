# Main Application Composition

## Purpose

`main/main.c` is the firmware composition root. It initializes platform,
display, storage, GUI, input, Wi-Fi, sensor, Firebase Authentication, and cloud
components in dependency order. It maps manager-owned snapshots into copied GUI
and cloud data without calling LVGL or HTTPS from producer callbacks.

Reusable component ownership is documented in [`components/README.md`](../components/README.md).

## Version 1 Status

```text
Release: v1.0.0
Status: Implemented and hardware accepted
```

## Startup Order

1. Log project name, semantic version, and release date.
2. Initialize NVS, `config_manager`, ESP-NETIF, and the default event loop.
3. Initialize the ST7735 display and LVGL integration.
4. Mount microSD and register the LVGL `S:` filesystem.
5. Initialize `app_gui` and start the single UI task.
6. Optionally start `performance_monitor`.
7. Initialize/start the reset coordinator and button manager.
8. Initialize `wifi_manager` and its status callback.
9. Initialize the application network coordinator.
10. Initialize `firebase_auth` and `cloud_manager`; create the telemetry queue
    without starting TLS.
11. Initialize/start `sensor_manager`.
12. Schedule the one-shot network coordinator.
13. Start `cloud_manager` only after stored-Wi-Fi startup or successful
    provisioning cleanup and Station adoption.

## Runtime Event Flow

```text
wifi_manager callback
    -> network coordinator runtime event
    -> cloud network epoch / IPv4 snapshot
    -> copied Wi-Fi GUI model

sensor_manager callback
    -> copied sensor GUI model
    -> copied latest-value cloud telemetry

cloud_manager callback
    -> copied cloud GUI model

button_manager callback
    -> copied reset input event
    -> reset coordinator task

provisioning_manager callback
    -> copied progress/credential handoff
    -> network coordinator persistence and adoption
```

Callbacks return quickly. The GUI task owns LVGL; the cloud task owns Firebase
Authentication and HTTPS; the reset coordinator owns the ordered persistent
cleanup transaction.

## Main Configuration

- DHT22 sample period: 2000 ms.
- Sensor stale timeout: 10000 ms.
- Cloud successful publish period: 10000 ms.
- Firebase token refresh margin: 300 seconds.
- Telemetry path: `devices/esp32s3-001/latest.json`.
- Provisioning session timeout: 120 seconds.
- Provisioning IPv4 grace: 30 seconds.
- Maximum provisioning sessions: 3.
- Cloud retry: 5 seconds to 60 seconds.
- Factory-reset input: active-low GPIO9, five-second hold.

Firebase API key, device email, password, and optional expected UID come from
local project Kconfig values in the generated, Git-ignored `sdkconfig`. They are
compiled into development firmware and are not a production secret-storage
mechanism.

## Ownership Rules

- `main` composes services but does not own reusable domain logic.
- `app_network_coordinator` owns boot/config-driven network policy.
- `wifi_manager` owns Station connection, reconnect, and driver persistence.
- `provisioning_manager` owns temporary BLE transport and credential handoff.
- `config_manager` owns durable application configuration.
- `sensor_manager` owns DHT22 sampling.
- `firebase_auth` owns sign-in and token lifecycle.
- `cloud_manager` owns authenticated telemetry and retry.
- `app_gui` and `ui_manager_lvgl` own screens and LVGL synchronization.
- `button_manager` publishes input events only.
- `app_reset_coordinator` owns reset qualification and execution.

## Build And Run

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

Before building, complete:

- [`docs/SETUP.md`](../docs/SETUP.md)
- [`components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md`](../components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)

## Security Notes

- Do not hard-code or log Wi-Fi credentials, Firebase passwords, ID tokens, or
  refresh tokens.
- Do not publish a local `sdkconfig` or a firmware binary built with real
  credentials.
- Realtime Database rules must deny anonymous access and authorize the intended
  device UID only.
- The database URL and device ID are deployment identifiers; move them to a
  protected deployment configuration before fleet or production use.

See [`SECURITY.md`](../SECURITY.md).
