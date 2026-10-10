# Setup And Build Guide

## Supported Target

This release targets an ESP32-S3 N16R8 with 16 MB flash and 8 MB PSRAM using
ESP-IDF 6.0.1. Use the current
[`board_config.h`](../components/system/common/include/board_config.h) for the
authoritative wiring values; do not duplicate pins in application code.

## Hardware Requirements

- ESP32-S3 N16R8 board and USB data cable
- ST7735 128×160 SPI display
- DHT22 sensor
- microSD card formatted for FAT
- INMP441 microphone and MAX98357A amplifier/speaker for V2 audio
- Buttons for GPIO9 factory reset and GPIO38 PTT
- One NeoPixel on GPIO48 when light control is used

Current board mapping includes LCD SPI2 on GPIO10–15, SD SPI3 on GPIO8 and
GPIO16–18, DHT22 on GPIO4, reset on GPIO9, PTT on GPIO38, NeoPixel on GPIO48,
and I2S audio on GPIO47/21/2/7. GPIO38 is active-high and requires the
documented external 10 kOhm pull-down for a reliable physical switch.

## Install ESP-IDF

Install ESP-IDF 6.0.1 using Espressif's official instructions. In PowerShell,
activate the matching environment before invoking `idf.py`:

```powershell
. C:\esp\v6.0.1\esp-idf\export.ps1
```

Use the actual installation path on the development machine. Do not copy a
local `sdkconfig` containing credentials between machines or into Git.

## Clone, Configure, And Build

```powershell
git clone <repository-url>
cd Smart_Room_Cloud_Gateway
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

The project requires the configured flash/PSRAM target and its partition table.
Avoid changing partition, Kconfig, or dependency files solely to make a local
build appear smaller.

## SD Card Preparation

Use a FAT-formatted card. The firmware mounts it at `/sdcard`; the current
board configuration uses a conservative 4 MHz SDSPI clock. Keep WAV tracks in
the product audio directory expected by the Local Web/MCP catalog. The audio
path accepts PCM16 mono 16 kHz WAV files smaller than 2 GiB.

SD mount/recovery remains asynchronous. A card that is absent, damaged, or
removed during use can make storage features unavailable without making the
rest of the gateway a storage owner.

## Firebase Development Configuration

Set Firebase development values only through Menuconfig:

```text
Smart Room Cloud Gateway
  -> Firebase development configuration
```

The API key, dedicated device email/password, and optional expected UID are
compiled into local development firmware. They are not production secret
storage. Never commit the generated `sdkconfig`, firmware binaries, tokens, or
database screenshots containing private data. See
[Firebase setup and security](../components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md).

## Project Menuconfig

```text
Smart Room Cloud Gateway
  -> Firebase development configuration
  -> Local Web Server
  -> Audio Manager
  -> Light Manager -> Validation / Debug options
  -> Application Log Manager
  -> Performance Monitor
  -> Xiaozhi / Voice Assistant
```

`LOCAL_WEB_SERVER_ENABLE`, logging, and performance monitoring are product
configuration choices. Keep the Light Manager infinite validation loop disabled
outside an intentional target regression run.

## Xiaozhi Prerequisites

Prepare the Xiaozhi account/agent and network access required by the managed
provider before attempting a voice session. Provider enrollment and console
procedures are external-service operations: follow the current instructions
provided by Xiaozhi rather than relying on undocumented website navigation in
this repository. Do not publish activation codes, session data, or account
information.

## Xiaozhi First-Boot Activation

The default option is:

```text
Smart Room Cloud Gateway
  -> Xiaozhi / Voice Assistant
      -> Enable Xiaozhi first-boot activation flow
```

The Kconfig symbol is `CONFIG_SMART_ROOM_XIAOZHI_FIRST_BOOT_ACTIVATION` and it
defaults to enabled.

1. Boot the configured device and allow its normal network/audio/voice startup
   policy to reach the Xiaozhi lifecycle.
2. If the backend reports that activation is required, the firmware publishes a
   bounded copied code and optional message to its voice UI model and serial
   log, while leaving unrelated gateway services running.
3. Register the device using the code through the current Xiaozhi-provided
   account/agent flow. Do not publish the code.
4. The firmware rechecks `get_info` on its lifecycle-owned 10/20/40/60-second
   capped schedule. A reboot is not required for the next recheck; a power
   cycle simply performs the same backend-authoritative check again.

While activation is required, WebSocket/MCP session startup and PTT capture are
not authorized. The firmware does not write an "already activated" marker to
NVS, so factory reset or erase-flash does not override the backend result. To
retain the previous compatibility behavior for a controlled test build, disable
the option in Menuconfig or use `sdkconfig.activation_disabled.defaults` as a
build profile; do not use that profile for normal deployment.

Verify readiness from the copied voice/session status after the backend no
longer requests activation. Common failures are missing network connectivity,
incorrect provider/account enrollment, an expired/wrong activation code, or
the backend continuing to request activation. Capture sanitized logs and check
the external service account before changing firmware.

## Flash And Monitor

Use the actual serial port:

```powershell
idf.py -p <PORT> flash monitor
```

On first boot, either stored Wi-Fi configuration is loaded or BLE provisioning
is started. Follow the displayed provisioning flow, then allow Station
association and IPv4 acquisition before expecting cloud, Local Web, audio, or
voice startup.

## Local Web, PTT, And MCP Checks

When the gateway is online and Local Web is enabled, open the address reported
by the device on the same trusted network. Exercise storage, audio, light,
dashboard, scenes, logs, and diagnostics with sanitized test data. Remote PTT
is a device-microphone control surface: the browser must not be treated as an
audio capture client.

For Xiaozhi, verify the session reaches READY before using GPIO38 PTT. MCP
queries return copied state; action admission such as `cloud.push_latest` or
playback scheduling is not proof that the later cloud/audio operation completed.

## Troubleshooting

| Symptom | Check |
|---|---|
| No display | Verify ST7735 wiring and board mapping before changing LVGL code. |
| SD unavailable | Verify card format, SPI wiring, and card health; wait for recovery status. |
| Local Web unavailable | Confirm `LOCAL_WEB_SERVER_ENABLE`, Wi-Fi/IPv4, and same-network access. |
| No PTT session | Confirm GPIO38 wiring/pull-down, Xiaozhi READY state, and no activation requirement. |
| Activation code never clears | Check current Xiaozhi account/agent registration and backend response; do not store a local bypass. |
| Cloud upload fails | Check sanitized Firebase configuration, network state, and restrictive database rules. |

## Resetting Local Build State

Use `idf.py fullclean` only when a clean build is needed. It removes generated
build output, not source history. Re-enter local Menuconfig secrets afterward
if the generated configuration has been removed.
