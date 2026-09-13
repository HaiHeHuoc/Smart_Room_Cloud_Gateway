# Component Organization

This directory groups ESP-IDF components by technical domain. Domain folders are
organizational containers only; ESP-IDF components live one level below them.
A large component may additionally contain private `modules/` directories. Those
private modules are implementation units owned by the parent component, not
independent ESP-IDF components.

## Top-Level Structure

```text
components/
|-- application/
|   |-- app_network_coordinator/
|   |-- app_reset_coordinator/
|   |-- smart_room_app/
|   |-- smart_room_mcp_adapter/
|   |-- voice_assistant/
|   `-- xiaozhi_foundation/
|-- audio/
|   `-- audio_manager/
|-- cloud/
|   |-- cloud_manager/
|   `-- firebase_auth/
|-- connectivity/
|   |-- provisioning_manager/
|   `-- wifi_manager/
|-- display/
|   |-- display_driver/
|   `-- waveshare__esp_lcd_st7735/
|-- input/
|   `-- button_manager/
|-- output/
|   |-- light_manager/
|   `-- neopixel/
|-- sensing/
|   |-- sensor_manager/
|   `-- sensor_DHT22/
|-- storage/
|   |-- config_manager/
|   `-- sd_card_manager/
|-- system/
|   |-- app_log/
|   |-- common/
|   |-- log_manager/
|   |-- performance_monitor/
|   `-- time_manager/
`-- ui/
    |-- app_gui/
    |-- lvgl_image_handler/
    |-- lvgl_sd_fs/
    `-- ui_manager_lvgl/
```

The project-level `CMakeLists.txt` registers the domain directories through
`EXTRA_COMPONENT_DIRS`. Domain folders do not define facade APIs, tasks, queues,
mutexes, or runtime lifecycle.

## Large Components And Private Modules

When one component owns several tightly coupled internal subsystems, those
subsystems are grouped below `modules/` instead of leaving unrelated `.c/.h`
files loose at the component root.

Typical layout:

```text
large_component/
|-- CMakeLists.txt
|-- include/
|   `-- large_component.h          # public API
|-- large_component.c              # facade / lifecycle owner
`-- modules/
    |-- subsystem_a/
    |   |-- include/               # parent-private headers
    |   `-- src/
    `-- subsystem_b/
        |-- include/
        `-- src/
```

Current examples include:

- `audio_manager/modules/{arbitration,playback,stream,dsp,wav}`;
- `log_manager/modules/{buffer,console}`;
- `cloud_manager/modules/telemetry_json`;
- `voice_assistant/modules/{audio,ui,ptt,codec,uplink,downlink}`;
- `smart_room_mcp_adapter/modules/provider`;
- `xiaozhi_foundation/modules/` for session, transport/text and production MCP
  tool internals;
- UI-private allocator, GIF, and theme modules.

The former `audio_manager/modules/test_support` production-tree support was
retired during the application-structure cleanup and is not a current module.

Private-module rules:

1. The parent component owns build, lifecycle, synchronization, and public API.
2. A private module normally has no `CMakeLists.txt` of its own.
3. Parent CMake lists private module sources in `SRCS` and private headers in
   `PRIV_INCLUDE_DIRS`.
4. Other ESP-IDF components must not include headers from another component's
   `modules/` directory.
5. Promote a private module into a standalone ESP-IDF component only when it
   gains a real independent lifecycle or reuse requirement.

## Ownership

| Domain | Components | Primary responsibility |
|---|---|---|
| Application | `smart_room_app`, `smart_room_mcp_adapter`, `app_network_coordinator`, `app_reset_coordinator`, `voice_assistant`, `xiaozhi_foundation` | Product composition, bounded MCP-domain adaptation, reset/network policy, voice conversation lifecycle, and the managed Xiaozhi/MCP boundary |
| Audio | `audio_manager` | I2S microphone/speaker ownership, audio arbitration, streaming, DSP/WAV processing, recording/playback and copied diagnostics |
| Cloud | `cloud_manager`, `firebase_auth` | Firebase telemetry policy and authentication/token lifecycle |
| Connectivity | `provisioning_manager`, `wifi_manager` | BLE provisioning and Wi-Fi Station lifecycle |
| Display | `display_driver`, `waveshare__esp_lcd_st7735` | Current-board LCD integration and ST7735 panel implementation |
| Input | `button_manager` | Debounced button input and event publication |
| Output | `light_manager`, `neopixel` | Product light state/effects and reusable WS2812/RMT driver behavior |
| Sensing | `sensor_manager`, `sensor_DHT22` | Sampling/staleness policy and DHT22 acquisition |
| Storage | `config_manager`, `sd_card_manager` | Persistent application configuration and managed SD/VFS lifecycle |
| System | `app_log`, `common`, `log_manager`, `performance_monitor`, `time_manager` | Logging frontend/backend, project board/config glue, diagnostics, and system time |
| UI | `app_gui`, `ui_manager_lvgl`, `lvgl_image_handler`, `lvgl_sd_fs` | Product screens, LVGL synchronization/runtime, image handling, and SD filesystem adaptation |

## Application Composition Split

The current application structure intentionally separates three responsibilities:

```text
main/main.c
    thin ESP-IDF entrypoint

smart_room_app
    product startup/order/policy/callback routing

smart_room_mcp_adapter
    Smart Room public service <-> Xiaozhi provider adaptation

xiaozhi_foundation
    managed esp_xiaozhi/MCP engine and session boundary
```

This avoids putting loose MCP provider adapters, historical phase wrappers, and
test coordinators back into `main/`.

## Reuse Levels

### Reusable component library

Examples: `app_log`, `wifi_manager`, `provisioning_manager`, `config_manager`,
`time_manager`, `button_manager`, `firebase_auth`, `performance_monitor`, and
the ST7735 panel component.

Target: copy into another ESP-IDF project, provide documented configuration and
normal framework dependencies, and build without editing internal source.

### Platform/service component

Examples: `audio_manager`, `log_manager`, `sd_card_manager`, `display_driver`,
`sensor_manager`, `sensor_DHT22`, `xiaozhi_foundation`, and LVGL adapters.

Target: reuse on the same ESP-IDF platform after supplying board/provider
configuration. Explicit board integration is allowed where owned by the correct
layer.

### Product/application component

Examples: `smart_room_app`, `smart_room_mcp_adapter`, `app_gui`,
`app_network_coordinator`, `app_reset_coordinator`, `voice_assistant`, and
application telemetry policy in `cloud_manager`.

These may remain Smart-Room-specific. Do not generalize them merely to increase
a portability score.

## Dependency Rules

- Dependency direction is `application -> service -> driver/framework`.
- Reusable lower-level components must not depend on `app_gui` or application
  coordinators.
- A dependency whose type/header appears in a public header belongs in CMake
  `REQUIRES`; implementation-only dependencies belong in `PRIV_REQUIRES`.
- `board_config.h` remains the Smart Room source of truth for physical board
  mapping.
- `app_common.h` is application configuration/identity, not a generic utility
  dumping ground.
- Equal numeric values in different APIs are not a reason to centralize
  independent contracts.
- `app_log` stays independent from persistent `log_manager`.
- Managed Xiaozhi handles, provider enums, credentials, callback-lifetime
  pointers, and transport objects must not escape `xiaozhi_foundation`.
- `smart_room_mcp_adapter` may register providers with `xiaozhi_foundation`, but
  it must not retain managed Xiaozhi/MCP handles or bypass domain owners.
- `managed_components/` remains owned by ESP-IDF Component Manager.
- Cross-domain facade APIs or roadmap-level ownership changes require a
  separate design decision.

## Portability Acceptance Rule

For a component classified as reusable, the desired practical test is:

> Copy the component/package into a minimal ESP-IDF project, configure its
> documented hardware/framework dependencies through public configuration or
> Kconfig, call its public API, and build without modifying component internals.

ESP-IDF/ESP32 reuse is the target; cross-platform STM32, Zephyr, or Linux
portability is not the objective of this repository.

## Documentation Index

- Application entry/composition: [`main/README.md`](../main/README.md)
- Product composition: [`application/smart_room_app/README.md`](application/smart_room_app/README.md)
- Smart Room MCP adapter: [`application/smart_room_mcp_adapter/README.md`](application/smart_room_mcp_adapter/README.md)
- System architecture: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md)
- Build and hardware setup: [`docs/SETUP.md`](../docs/SETUP.md)
- Xiaozhi boundary: [`application/xiaozhi_foundation/docs/README.md`](application/xiaozhi_foundation/docs/README.md)
- Voice assistant: [`application/voice_assistant/README.md`](application/voice_assistant/README.md)
- Audio manager: [`audio/audio_manager/docs/README.md`](audio/audio_manager/docs/README.md)
- Firebase Authentication: [`cloud/firebase_auth/docs/README.md`](cloud/firebase_auth/docs/README.md)
- Firebase setup/security: [`cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md`](cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)
- Cloud telemetry: [`cloud/cloud_manager/docs/README.md`](cloud/cloud_manager/docs/README.md)

Other component-specific behavior and limitations remain in each component's
root `README.md` or `docs/README.md`.
