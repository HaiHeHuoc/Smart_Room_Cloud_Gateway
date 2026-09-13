# Main Application Composition

## Purpose

`main/main.c` is the firmware entrypoint and deliberately contains only the
top-level call to `smart_room_app_start()`.

The product composition component `smart_room_app` owns startup ordering,
application policy values, and copied manager-to-GUI/cloud callback routing. It
does not take ownership of manager/driver implementations, LVGL, I2S, network
stacks, or the Xiaozhi/MCP engine.

Reusable component ownership is documented in [`components/README.md`](../components/README.md).

## Source Structure

```text
main/
  main.c                         ESP-IDF app_main() entrypoint only
components/application/
  smart_room_app/                product composition/startup/callback routing
  smart_room_mcp_adapter/        Smart Room domain <-> MCP provider adaptation
  xiaozhi_foundation/            managed Xiaozhi/MCP engine/session boundary
  voice_assistant/               product voice-session orchestration
```

`smart_room_app` registers local service callbacks and the bounded Smart Room
MCP provider set before production voice startup. `xiaozhi_foundation` remains
the sole direct managed Xiaozhi/MCP boundary and attaches the production MCP
tools when its session is started.

## Startup Order

1. Initialize persistent logging when enabled and emit project identity/version.
2. Initialize `light_manager` with board-owned NeoPixel mapping.
3. Initialize NVS, `config_manager`, ESP-NETIF, and the default event loop.
4. Initialize `time_manager`.
5. Initialize the ST7735 display and LVGL runtime.
6. Prepare SD recovery and register the LVGL `S:` filesystem while offline.
7. Initialize/start `app_gui` and present the built-in BOOT screen.
8. Start the background SD recovery task when initialization succeeded.
9. Initialize/start reset coordination and button input.
10. Initialize `wifi_manager` and application network coordination.
11. Initialize Firebase Authentication and `cloud_manager` without starting
    cloud transport before network ownership permits it.
12. Initialize/start `sensor_manager`.
13. Register Smart Room MCP providers through `smart_room_mcp_adapter`.
14. Start the one-shot network coordinator.
15. During lifecycle polling, start cloud and audio/voice only after the
    coordinator reaches the required online/handoff state.

The exact call order in current source is authoritative when this summary and
implementation ever disagree.

## Runtime Event Flow

```text
wifi_manager callback
    -> app_network_coordinator runtime event
    -> cloud network epoch / IPv4 snapshot
    -> copied Wi-Fi GUI model

sensor_manager callback
    -> copied sensor GUI model
    -> copied latest-value cloud telemetry

audio_manager callback
    -> copied audio GUI model
    -> app_gui task renders status

cloud_manager callback
    -> copied cloud GUI model

button_manager callback
    -> copied reset input event
    -> app_reset_coordinator task

Smart Room MCP tool
    -> xiaozhi_foundation
    -> smart_room_mcp_adapter provider
    -> owning manager/service
```

Callbacks return quickly. The GUI task owns LVGL; the cloud task owns Firebase
Authentication/HTTPS work; domain managers own their hardware/resources.

## Application Configuration

Current product-level values include:

- DHT22 sample period: 3000 ms.
- Sensor stale timeout: 10000 ms.
- Cloud successful publish period: 60000 ms.
- Firebase token refresh margin: 300 seconds.
- Telemetry path: `devices/esp32s3-001/latest.json`.
- Provisioning session timeout: 120 seconds.
- Provisioning IPv4 grace: 30 seconds.
- Maximum provisioning sessions: 3.
- Factory-reset input: board-configured GPIO9, active-high, five-second hold.
- NeoPixel: board-configured GPIO48, one GRB pixel, initial logical brightness
  100%.
- Audio startup gate: application network coordinator `ONLINE` after valid IPv4
  and any provisioning cleanup/Station adoption.
- Normal `audio_manager` startup is production `IDLE`; retired public-API/golden
  stress coordinators are not selectable from the current product Kconfig.

Firebase API key, device email, password, and optional expected UID come from
local project Kconfig values in generated Git-ignored `sdkconfig`. They are
compiled into development firmware and are not a production secret-storage
mechanism.

## Ownership Rules

- `main` only enters `smart_room_app`.
- `smart_room_app` composes services and callback routes; it does not own
  reusable domain logic or hardware resources.
- `smart_room_mcp_adapter` maps copied/public service state into registered
  Xiaozhi provider callbacks and invokes only public owner APIs.
- `xiaozhi_foundation` owns the direct managed Xiaozhi/MCP engine/session
  boundary; provider handles and framework-lifetime pointers do not escape it.
- `app_network_coordinator` owns boot/provisioning/network handoff policy.
- `wifi_manager` owns Station connection/reconnect.
- `provisioning_manager` owns temporary BLE provisioning transport.
- `config_manager` owns durable application configuration.
- `sensor_manager` owns DHT22 sampling/staleness.
- `audio_manager` owns microphone/speaker I2S, DMA, PCM buffers, recording and
  playback resources.
- `light_manager` owns product light state/effects; NeoPixel/RMT stays below it.
- `sd_card_manager` owns SDSPI/FAT VFS lifecycle/recovery and leases.
- `firebase_auth` owns sign-in/token lifecycle.
- `cloud_manager` owns authenticated telemetry and retry.
- `app_gui` and `ui_manager_lvgl` own screens and LVGL synchronization.
- `button_manager` publishes input events only.
- `app_reset_coordinator` owns reset qualification/execution.

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
- Do not publish a local `sdkconfig` or firmware binary built with real
  credentials.
- Realtime Database authorization must be restrictive for the intended device
  identity.

See [`SECURITY.md`](../SECURITY.md).
