# ESP32-S3 Smart Room Cloud Gateway

An ESP-IDF smart-room gateway for the ESP32-S3 N16R8 with an LVGL dashboard,
BLE Wi-Fi provisioning, DHT22 monitoring, authenticated Firebase telemetry,
local audio/voice, Xiaozhi integration, bounded MCP tools, persistent
configuration, recovery services, and runtime diagnostics.

```text
Version 1: v1.0.0 / hardware accepted baseline
Version 2: active development through Sprint 18
Current Phase 18 state: 18.1 complete; 18.2-18.4 not started
Target: ESP32-S3 N16R8
Framework: ESP-IDF 6.0.1 + FreeRTOS
```

## Product Demo

[![Watch the ESP32-S3 Smart Room Cloud Gateway demo](docs/media/screen-dashboard.jpg)](https://youtube.com/shorts/9C5_hecEgXA?feature=share)

> Select the dashboard image to watch the Version-1 target-hardware demo.

| Prototype | BLE provisioning |
|---|---|
| ![Wired ESP32-S3 prototype](docs/media/hardware-overview.jpg) | ![BLE provisioning screen](docs/media/screen-provisioning.jpg) |

| Wi-Fi status | Sensor dashboard | Factory reset result |
|---|---|---|
| ![Wi-Fi status](docs/media/screen-wifi.jpg) | ![Sensor dashboard](docs/media/screen-dashboard.jpg) | ![Factory-reset result](docs/media/screen-reset-result.jpg) |

See the [media index](docs/media/README.md) for the evidence list and public
sanitization rules.

## Current Product Capabilities

Version 1 established the local gateway baseline:

- ST7735 128x160 SPI display with LVGL 9;
- DHT22 temperature/humidity sampling with stale/error handling;
- BLE Security 1 Wi-Fi provisioning and NVS-backed configuration;
- Wi-Fi Station reconnect and bounded network coordination;
- Firebase Email/Password authentication and authenticated RTDB telemetry;
- GPIO9 five-second factory reset and reboot-to-provisioning recovery;
- SD/FAT VFS integration, logging/recovery services, and runtime diagnostics.

Version 2 adds the current audio/voice/Xiaozhi path:

- production `audio_manager` ownership of microphone/speaker I2S, DMA, PCM,
  recording and playback;
- project-owned `voice_assistant` orchestration over `xiaozhi_foundation`;
- Xiaozhi WebSocket voice transport and streaming downlink;
- MCP read-only Smart Room tools;
- Phase-18.1 bounded NeoPixel light control through `light.set_state`, with
  `light.get_state` and `light.get_capabilities` companions.

Phase 18.2-18.4 remain not started until explicitly requested.

## Current Application Structure

```text
main/main.c
    -> smart_room_app
        -> product startup/order/policy/callback routing
        -> smart_room_mcp_adapter
            -> Smart Room provider adaptation
            -> xiaozhi_foundation
                -> managed Xiaozhi / MCP engine and session
```

The source is organized by ownership rather than by historical phase number.
`main/main.c` is intentionally only the ESP-IDF entrypoint. Product composition
lives in `smart_room_app`; MCP-domain provider bridges live in
`smart_room_mcp_adapter`; the managed Xiaozhi/MCP lifecycle remains inside
`xiaozhi_foundation`.

See [Architecture](docs/ARCHITECTURE.md) and
[Component organization](components/README.md) for the detailed dependency and
ownership model.

### Ownership Boundaries

| Component | Responsibility |
|---|---|
| `smart_room_app` | Product startup order, product policy, copied callback routing |
| `smart_room_mcp_adapter` | Smart Room public-service to MCP-provider adaptation |
| `xiaozhi_foundation` | Direct managed Xiaozhi/MCP engine and session boundary |
| `voice_assistant` | Product voice-session and recovery orchestration |
| `audio_manager` | Microphone/speaker I2S, DMA, PCM, recording/playback ownership |
| `wifi_manager` | Wi-Fi Station lifecycle and reconnect |
| `provisioning_manager` | Temporary BLE provisioning transport |
| `config_manager` | Persistent application configuration |
| `sensor_manager` | DHT22 sampling and sensor state |
| `firebase_auth` | Sign-in/token cache/refresh lifecycle |
| `cloud_manager` | Latest-value telemetry and retry policy |
| `light_manager` | Product light state and effects; NeoPixel remains below it |
| `app_gui` / `ui_manager_lvgl` | Screens, copied UI models, LVGL ownership |
| `sd_card_manager` | SD/FAT VFS lifecycle, recovery and leases |
| `button_manager` | Debounced input event publication |
| `app_reset_coordinator` | Ordered factory-reset transaction |

Dependency direction remains:

```text
application -> service -> driver/framework
```

Callbacks copy bounded data and return quickly. No arbitrary producer callback
may directly own LVGL, I2S, Wi-Fi lifecycle, persistent reset, or unrelated
hardware resources.

## Hardware

| Device | Purpose |
|---|---|
| ESP32-S3 N16R8 | Main controller, 16 MB flash, 8 MiB Octal PSRAM |
| ST7735 128x160 TFT | LVGL dashboard and provisioning UI |
| Integrated microSD slot/card | FAT filesystem, assets, WAV/log storage |
| DHT22 | Temperature and humidity |
| GPIO9 active-high push button | Five-second factory reset |
| GPIO38 active-high push button | Push-To-Talk input |
| INMP441 | I2S microphone |
| MAX98357A | I2S speaker amplifier/output |
| GPIO48 NeoPixel | Product light/status output |

### GPIO Map

| Function | GPIO |
|---|---:|
| LCD MOSI | 11 |
| LCD SCLK | 12 |
| LCD CS | 10 |
| LCD DC | 13 |
| LCD RST | 14 |
| LCD backlight | 15 |
| SD MOSI | 16 |
| SD MISO | 17 |
| SD SCLK | 18 |
| SD CS | 8 |
| DHT22 data | 4 |
| Factory-reset button | 9 |
| PTT button | 38 |
| NeoPixel | 48 |
| Audio BCLK | 47 |
| Audio WS/LRCLK | 21 |
| INMP441 DIN | 2 |
| MAX98357A DOUT | 7 |

Always confirm the source of truth in
[`board_config.h`](components/system/common/include/board_config.h) before
rewiring hardware.

## Runtime Flow

Base gateway startup:

```text
boot
-> smart_room_app composition
-> local display/storage/config/input services
-> Wi-Fi/provisioning coordination
-> sensor/cloud services
-> audio/voice startup after network handoff
```

Voice/MCP path:

```text
PTT / microphone
-> audio_manager
-> voice_assistant
-> xiaozhi_foundation / Xiaozhi WebSocket session
-> backend ASR/LLM
-> optional MCP tool call
-> smart_room_mcp_adapter provider
-> owning manager/service
-> result returned through Xiaozhi
```

Phase-18.1 light control never drives GPIO/RMT directly from MCP;
`light_manager` remains the product owner.

## Quick Start

### Requirements

- ESP-IDF 6.0.1;
- ESP32-S3 N16R8 and the required peripherals;
- FAT-formatted microSD card when SD-backed assets/WAV/logging are needed;
- Firebase project with Email/Password Authentication and Realtime Database;
- Xiaozhi backend/activation configuration for the current voice path.

### Configure And Build

```bash
git clone https://github.com/HaiHeHuoc/Smart_Room_Cloud_Gateway.git
cd Smart_Room_Cloud_Gateway
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p <PORT> flash monitor
```

Configure Firebase under:

```text
Smart Room Cloud Gateway
└── Firebase development configuration
```

Complete guides:

- [Hardware, build, flash, and first-boot setup](docs/SETUP.md)
- [Firebase Authentication and Security setup](components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)
- [Xiaozhi roadmap](XIAOZHI_IMPLEMENTATION_ROADMAP.md)

## Security Model

Development Firebase values are entered through local `menuconfig` and generated
into Git-ignored `sdkconfig`. They are still compiled into development firmware
and are not a production secret-storage mechanism.

Never commit or publish:

- Firebase passwords, tokens, or private service-account material;
- Wi-Fi credentials or provisioning secrets;
- activation/session secrets or private transport payloads;
- `sdkconfig` containing real local values;
- firmware binaries built with real credentials.

See [SECURITY.md](SECURITY.md).

## Memory / Voice Notes

Current voice configuration uses dynamic mbedTLS buffers in PSRAM and a 1 KiB
outbound TLS record. PTT performs a 20 KiB total/largest-contiguous PSRAM
headroom gate before starting the turn.

Current streaming-downlink behavior uses a bounded 7.68-second PSRAM ingress
ring and 0.96-second normal prefill. The five-second prefill deadline starts
after the first PCM packet rather than at `TTS_START`.

These are current implementation facts, not generic ESP-IDF requirements.

## Repository Layout

```text
main/                     thin ESP-IDF entrypoint and project-level config
components/application/  product composition, MCP adapter, coordinators, voice/Xiaozhi
components/audio/        audio manager and private audio modules
components/cloud/        Firebase authentication and telemetry
components/connectivity/ Wi-Fi and BLE provisioning
components/display/      ST7735/display integration
components/input/        button input
components/output/       light manager and NeoPixel driver
components/sensing/      DHT22 and sensor manager
components/storage/      NVS configuration and SD card
components/system/       shared board/config/logging/time/diagnostics
components/ui/           LVGL runtime, screens, filesystem and image handling
docs/                    architecture, setup, demo, limitations and media
AI_Stored_Data/          AI handoff/support metadata; never a runtime dependency
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Component organization](components/README.md)
- [Application composition](main/README.md)
- [Version 1 release record](VERSION_1_RELEASE.md)
- [Setup and build](docs/SETUP.md)
- [Firebase setup and security](components/cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)
- [Xiaozhi foundation](components/application/xiaozhi_foundation/docs/README.md)
- [Voice assistant](components/application/voice_assistant/README.md)
- [Audio manager](components/audio/audio_manager/docs/README.md)
- [Known limitations and future work](docs/KNOWN_LIMITATIONS.md)
- [Historical Version-1 roadmap](ESP32S3_Smart_Room_Cloud_Gateway_Roadmap.md)
- [Version-2 Xiaozhi roadmap](XIAOZHI_IMPLEMENTATION_ROADMAP.md)

## Current Roadmap Status

```text
Sprint 17   COMPLETE / read-only MCP voice HIL accepted
Sprint 18   IN PROGRESS
Phase 18.1  COMPLETE / build PASS / target HIL accepted by Hải on 2026-09-13
Phase 18.2  NOT STARTED
Phase 18.3  NOT STARTED
Phase 18.4  NOT STARTED
Sprint 19   NOT STARTED
```

Historical phase documents remain historical evidence; current source and
current canonical project state take precedence when older status text differs.

## License

No explicit open-source license is currently included. Until the owner selects
one, the repository is source-available portfolio code under default copyright
rather than an open-source distribution.
