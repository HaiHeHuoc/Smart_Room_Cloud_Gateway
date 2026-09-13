# Main Application Composition

## Purpose

`main/main.c` is the firmware entrypoint and deliberately contains only the
top-level call to `smart_room_app_start()`. The product composition component
`smart_room_app` owns the established startup order, application policy values,
and copied manager-to-GUI/cloud callbacks. It does not take ownership of any
manager, driver, LVGL, I2S, or network implementation.

Reusable component ownership is documented in [`components/README.md`](../components/README.md).

## Version 1 Status

```text
Release: v1.0.0
Status: Implemented and hardware accepted
SD recovery extension: target-hardware acceptance pending
```

## Source Structure

```text
main/
  main.c                         ESP-IDF app_main() entrypoint only
components/application/
  smart_room_app/                product composition, startup and adapters
  smart_room_mcp_adapter/        domain snapshot providers for Xiaozhi MCP
test_apps/
  audio_legacy_test/             retired direct-I2S test source; not a product app
```

`smart_room_app` registers all local service callbacks before starting network
boot and registers MCP providers before a production voice session can start.
`xiaozhi_foundation` remains the sole managed Xiaozhi/MCP engine and session
lifecycle boundary. Production MCP tools are registered unconditionally when
that session starts.

## Startup Order

1. Log project name, semantic version, and release date.
2. Initialize NVS, `config_manager`, ESP-NETIF, and the default event loop.
3. Initialize the ST7735 display and LVGL integration.
4. Prepare the non-blocking SD recovery service and register the LVGL `S:`
   filesystem while it is still offline.
5. Initialize `app_gui`, start the single UI task, and present the built-in
   `Starting...` BOOT screen.
6. Start the background SD recovery task. A missing card is non-fatal; retry
   continues while the rest of the application starts.
7. Initialize/start the reset coordinator and button manager.
8. Initialize `wifi_manager` and its status callback.
9. Initialize the application network coordinator.
10. Initialize `firebase_auth` and `cloud_manager`; create the telemetry queue
   without starting TLS.
11. Initialize/start `sensor_manager`.
12. Schedule the one-shot network coordinator.
13. During lifecycle polling, start `cloud_manager` only after stored-Wi-Fi
   startup or successful provisioning cleanup and Station adoption.
14. In the same lifecycle polling, initialize `audio_manager`, register its
    copied GUI status adapter, and start its private I2S-owning task only after
    the coordinator reaches `ONLINE`. This reserves audio I2S/DMA/task
    allocation until Wi-Fi has a valid IPv4 address and any BLE provisioning
    cleanup and Station adoption have completed. During WAV playback, its
    private reader owns the file/SD VFS lease and bounded PSRAM cache. Normal
    startup is command-idle; the default-off golden/continuous-WAV stress
    hooks are selected only in Kconfig and start through the same `ONLINE`
    gate.

## Runtime Event Flow

```text
wifi_manager callback
    -> network coordinator runtime event
    -> cloud network epoch / IPv4 snapshot
    -> copied Wi-Fi GUI model

sensor_manager callback
    -> copied sensor GUI model
    -> copied latest-value cloud telemetry

audio_manager callback
    -> copied audio GUI model
    -> app_gui task renders dashboard status

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

## Application Configuration

- DHT22 sample period: 3000 ms.
- Sensor stale timeout: 10000 ms.
- Cloud successful publish period: 60000 ms.
- Firebase token refresh margin: 300 seconds.
- Telemetry path: `devices/esp32s3-001/latest.json`.
- Provisioning session timeout: 120 seconds.
- Provisioning IPv4 grace: 30 seconds.
- Maximum provisioning sessions: 3.
- Cloud retry: 5 seconds to 60 seconds.
- Factory-reset input: board-configured GPIO9, active-high, five-second hold.
- NeoPixel mapping: board-configured GPIO48 and one GRB pixel; initial logical
  brightness is 100%.
- Audio startup gate: coordinator `ONLINE` after a valid IPv4 address and, for
  a provisioned device, BLE cleanup plus Station adoption.
- Audio stability default: five-second recording, DSP, and playback at 100% volume.
  The current local WAV-only stress profile disables capture and replays the
  configured SD WAV file to completion at priority 6.
- SD initial settle/retry/deadline: 1 second / 2 seconds / 90 seconds.
- SD background retry and idle health-check intervals: 2 seconds / 5 seconds.

Firebase API key, device email, password, and optional expected UID come from
local project Kconfig values in the generated, Git-ignored `sdkconfig`. They are
compiled into development firmware and are not a production secret-storage
mechanism.

## Ownership Rules

- `main` only enters `smart_room_app`.
- `smart_room_app` composes services and application callback routes but does
  not own reusable domain logic.
- `smart_room_app` passes the board NeoPixel mapping from `board_config.h` to
  `light_manager`; it does not own GPIO/RMT or product light state.
- `app_network_coordinator` owns boot/config-driven network policy.
- `wifi_manager` owns Station connection, reconnect, and driver persistence.
- `provisioning_manager` owns temporary BLE transport and credential handoff.
- `config_manager` owns durable application configuration.
- `sensor_manager` owns DHT22 sampling.
- `audio_manager` owns I2S RX/TX, PCM stability buffers, and audio diagnostics;
  its private WAV reader owns the active VFS lease/file and bounded PSRAM cache.
- `smart_room_app` owns only the one-shot audio startup timing policy; it never starts
  audio from a Wi-Fi or provisioning callback.
- `sd_card_manager` owns SDSPI/FAT VFS lifecycle and background recovery;
  `lvgl_sd_fs` and private WAV playback hold managed VFS leases while files are
  open.
- `firebase_auth` owns sign-in and token lifecycle.
- `cloud_manager` owns authenticated telemetry and retry.
- `app_gui` and `ui_manager_lvgl` own screens and LVGL synchronization.
- `button_manager` publishes input events only.
- `app_reset_coordinator` owns reset qualification and execution.
- `smart_room_mcp_adapter` maps copied public service state to registered
  Xiaozhi provider callbacks; it never owns a driver, manager, or MCP session.

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
