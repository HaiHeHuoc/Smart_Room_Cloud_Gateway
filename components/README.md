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
`EXTRA_COMPONENT_DIRS`. ESP-IDF discovers each child component. Domain folders
do not define facade APIs, tasks, queues, mutexes, or runtime lifecycle.

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

- `audio_manager/modules/{arbitration,playback,stream,dsp,wav,test_support}`;
- `log_manager/modules/{buffer,console}`;
- `cloud_manager/modules/telemetry_json`;
- `voice_assistant/modules/{audio,ui,ptt,codec,uplink,downlink}`;
- `xiaozhi_foundation/modules/{session,text_bridge,websocket,fixture}`;
- UI-private allocator, GIF, and theme modules.

Private-module rules:

1. The parent component owns build, lifecycle, synchronization, and public API.
2. A private module normally has no `CMakeLists.txt` of its own.
3. Parent CMake lists private module sources in `SRCS` and private headers in
   `PRIV_INCLUDE_DIRS`.
4. Other ESP-IDF components must not include headers from another component's
   `modules/` directory.
5. Promote a private module into a standalone ESP-IDF component only when it
   gains a real independent lifecycle or reuse requirement.

This keeps large components understandable without recreating a dependency
graph made of many tiny components.

## Ownership

| Domain | Components | Primary responsibility |
|---|---|---|
| Application | `app_network_coordinator`, `app_reset_coordinator`, `voice_assistant`, `xiaozhi_foundation` | Product orchestration, reset/network policy, voice conversation lifecycle, and the project-owned Xiaozhi provider boundary |
| Audio | `audio_manager` | I2S microphone/speaker ownership, audio arbitration, streaming, DSP/WAV processing, and copied diagnostics |
| Cloud | `cloud_manager`, `firebase_auth` | Firebase telemetry policy and authentication/token lifecycle |
| Connectivity | `provisioning_manager`, `wifi_manager` | BLE provisioning and Wi-Fi Station lifecycle |
| Display | `display_driver`, `waveshare__esp_lcd_st7735` | Current board LCD integration and ST7735 panel implementation |
| Input | `button_manager` | Debounced button input and event publication |
| Output | `light_manager`, `neopixel` | Product static-light state and reusable WS2812/RMT driver behavior |
| Sensing | `sensor_manager`, `sensor_DHT22` | Sampling/staleness policy and DHT22 acquisition |
| Storage | `config_manager`, `sd_card_manager` | Persistent application configuration and managed SD/VFS lifecycle |
| System | `app_log`, `common`, `log_manager`, `performance_monitor`, `time_manager` | Logging frontend/backend, project board/config glue, diagnostics, and system time |
| UI | `app_gui`, `ui_manager_lvgl`, `lvgl_image_handler`, `lvgl_sd_fs` | Product screens, LVGL synchronization/runtime, image handling, and SD filesystem adaptation |

## Reuse Levels

Not every component is expected to have the same portability target.

### Reusable component library

Examples: `app_log`, `wifi_manager`, `provisioning_manager`, `config_manager`,
`time_manager`, `button_manager`, `firebase_auth`, `performance_monitor`, and
the ST7735 panel component.

Target: copy the component into another ESP-IDF project, declare normal
framework dependencies/configuration, and build without editing its internal
source.

### Platform/service component

Examples: `audio_manager`, `log_manager`, `sd_card_manager`, `display_driver`,
`sensor_manager`, `sensor_DHT22`, `xiaozhi_foundation`, and LVGL adapters.

Target: reuse on the same ESP-IDF platform after supplying board/provider
configuration. Board-specific integration is allowed when it is explicit and
owned by the correct layer.

### Product/application component

Examples: `app_gui`, `app_network_coordinator`, `app_reset_coordinator`,
`voice_assistant`, and the application telemetry policy in `cloud_manager`.

Target: clean dependency direction and ownership. These components are allowed
to remain Smart-Room-specific; forcing them into generic libraries would add
abstraction without useful reuse.

## Dependency Rules

- Dependency direction is `application -> service -> driver/framework`.
- Reusable lower-level components must not depend on `app_gui` or application
  coordinators.
- Public headers define the public dependency surface. A dependency whose type
  appears in a public header belongs in CMake `REQUIRES`; implementation-only
  dependencies belong in `PRIV_REQUIRES`.
- `board_config.h` remains the Smart Room source of truth for physical board
  mapping. Reusable leaf components should receive runtime configuration where
  doing so materially improves reuse; platform-integration components may use
  the centralized board mapping directly.
- `app_common.h` is application configuration/identity, not a generic utility
  dumping ground.
- Independent API limits may intentionally use equal numeric values; do not
  centralize constants only because their current values happen to match.
- `app_log` stays independent from persistent `log_manager`; logging producers
  can use the frontend without taking an SD/time backend dependency.
- Managed Xiaozhi handles, provider enums, credentials, callback-lifetime
  pointers, and other provider-owned resources must not escape the
  `xiaozhi_foundation` public boundary.
- `managed_components/` remains owned by ESP-IDF Component Manager.
- Cross-domain facade APIs or roadmap-level ownership changes require a
  separate design decision.

## Portability Acceptance Rule

For a component that is classified as reusable, the desired practical test is:

> Copy the component/package into a minimal ESP-IDF project, configure its
> documented hardware/framework dependencies through public configuration or
> Kconfig, call its public API, and build without modifying component internals.

A component can still be well designed even when it intentionally targets only
ESP-IDF/ESP32. Cross-platform STM32, Zephyr, or Linux portability is not the
objective of this repository.

## Documentation Index

- Application composition: [`main/README.md`](../main/README.md)
- System architecture: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md)
- Build and hardware setup: [`docs/SETUP.md`](../docs/SETUP.md)
- Xiaozhi boundary:
  [`application/xiaozhi_foundation/docs/README.md`](application/xiaozhi_foundation/docs/README.md)
- Voice assistant:
  [`application/voice_assistant/README.md`](application/voice_assistant/README.md)
- Audio manager: [`audio/audio_manager/docs/README.md`](audio/audio_manager/docs/README.md)
- Firebase Authentication component:
  [`cloud/firebase_auth/docs/README.md`](cloud/firebase_auth/docs/README.md)
- Firebase setup and security:
  [`cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md`](cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)
- Cloud telemetry component:
  [`cloud/cloud_manager/docs/README.md`](cloud/cloud_manager/docs/README.md)

Other component-specific behavior and limitations remain in each component's
root `README.md` or `docs/README.md`.
