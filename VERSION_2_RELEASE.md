# ESP32-S3 Smart Room Cloud Gateway — Version 2 Release

## Release Statement

Version 2.0.0 is the audio, voice, Xiaozhi, MCP, and Local Web extension of
the Version 1 Smart Room Cloud Gateway. This record describes the release
source tree; it does not assert that a GitHub Release, tag, or evidence package
has been published.

## Target Hardware

- ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM)
- ST7735 128×160 SPI display and DHT22 sensor
- microSD card interface
- INMP441 I2S microphone and MAX98357A I2S amplifier/speaker
- GPIO9 factory-reset button, GPIO38 push-to-talk button, and GPIO48 NeoPixel

The authoritative pin mapping is
[`board_config.h`](components/system/common/include/board_config.h).

## Framework And Dependencies

The firmware targets ESP-IDF 6.0.1 and FreeRTOS. Managed dependencies include
LVGL and `espressif/esp_xiaozhi` 0.1.2; exact resolved versions are in
`dependencies.lock`.

## Version 1 Baseline

Version 2 retains the Version 1 gateway: LCD/LVGL, BLE Wi-Fi provisioning,
Wi-Fi Station reconnect, DHT22 monitoring, authenticated Firebase Realtime
Database telemetry, NVS configuration, SD-card support, factory reset, logs,
and runtime diagnostics. See [the Version 1 release record](VERSION_1_RELEASE.md).

## Version 2 Scope

Version 2 adds local audio, Xiaozhi WebSocket voice sessions, bounded MCP
providers, a Local Web control surface, scenes, and NeoPixel light control.
Wake word and advanced always-on voice work are suspended and are not part of
this release.

## Major Features

- `audio_manager` owns microphone/speaker I2S, DMA, PCM buffers, recording,
  bounded WAV playback, and arbitration.
- `voice_assistant` coordinates GPIO38 and browser remote PTT without taking
  I2S ownership.
- `xiaozhi_foundation` owns the Xiaozhi WebSocket/MCP engine boundary, session
  recovery, activation status, and bounded tool registration.
- `smart_room_mcp_adapter` delegates product-domain MCP requests to public
  manager APIs.
- The Local Web UI provides bounded storage, playback, light, dashboard,
  scenes, logs, diagnostics, and remote-PTT control endpoints.

## Architecture

`main` enters `smart_room_app`, which composes services and copies callback
state. Component owners retain their hardware and lifecycle boundaries:
`audio_manager` owns I2S; `sd_card_manager` owns SDSPI/FAT VFS leases;
`ui_manager_lvgl` owns LVGL; `wifi_manager` owns Station reconnect; and
`xiaozhi_foundation` owns managed provider handles. See
[Architecture](docs/ARCHITECTURE.md).

## Audio And Voice Integration

Audio operations are serialized through the audio manager. WAV playback accepts
PCM16 mono 16 kHz files below 2 GiB and uses bounded PSRAM prefetch storage.
Voice requests use an explicit PTT ownership/lease policy. Request admission is
not proof of audible speaker output; hardware confirmation is separate.

## Xiaozhi Activation

First-boot activation is enabled by default through
`CONFIG_SMART_ROOM_XIAOZHI_FIRST_BOOT_ACTIVATION`. If the backend requests
activation, the firmware presents a bounded copied code/message, blocks PTT
and session startup, and rechecks backend state with bounded lifecycle retries.
It does not store an "activated" flag in NVS. See [Setup](docs/SETUP.md).

## MCP Tool Capabilities

The production session registers these bounded tools:

```text
smart_room.get_current_temperature_humidity
smart_room.get_cloud_sync_status
smart_room.get_system_status
cloud.push_latest
light.set_state / light.get_state / light.get_capabilities
audio.control_playback / audio.get_playback_state
audio.list_tracks / audio.play_track / audio.play_recorded
```

Query tools return copied snapshots. Controlled actions are delegated to their
owners; `cloud.push_latest` acknowledges scheduling only, not a completed
Firebase upload.

## Local Web Capabilities

When enabled and online, the Local Web server exposes bounded HTTP controls for
SD storage, audio, light state, dashboard, scenes, logs, diagnostics, and
remote PTT. It is a remote-control surface, not a browser microphone or direct
audio transport.

## Security Boundaries

This is portfolio/development firmware, not a production security baseline.
Do not publish local `sdkconfig`, credentials, activation/session data, raw
logs, or firmware binaries built with real configuration. See [Security](SECURITY.md).

## Configuration And Setup

Build configuration, wiring, BLE provisioning, Firebase configuration, Xiaozhi
activation, Local Web access, and verification steps are in [Setup](docs/SETUP.md).

## Validation Summary

Source-level component boundaries and host/build checks are documented with
their components. Some target behavior has owner-confirmed evidence; other
hardware, browser, endurance, and service-loss scenarios remain unverified.

## Hardware Acceptance Status

Version 1 hardware evidence is retained in `docs/media/`. Published Version 2
media, external video links, and any still-missing optional captures are listed
in [the V2 media manifest](docs/media/V2_MEDIA_ASSETS.md).

## Known Limitations And Deferred Features

See [Known Limitations](docs/KNOWN_LIMITATIONS.md). Deferred V2 work includes
wake word/VAD, secure boot and encryption, signed OTA, browser microphone
capture, and a 64-bit-safe WAV path for files at or above 2 GiB.

## Media And Product Evidence

Existing Version 1 media remains unchanged. Canonical Version 2 paths, external
video links, and capture requirements are listed in
[V2 Media Assets](docs/media/V2_MEDIA_ASSETS.md). Missing optional assets remain
explicitly identified there.

## Release Boundary

This branch contains the Version 2.0.0 release source and public
documentation. The owner performs final evidence review, tagging, GitHub
Release creation, package publication, and visibility decisions separately.
