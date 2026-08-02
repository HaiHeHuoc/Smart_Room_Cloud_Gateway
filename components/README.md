# Component Organization

This directory groups reusable ESP-IDF components by technical domain. Group
directories are organizational containers; each child remains an independent
ESP-IDF component with its own public API, dependencies, ownership, and
documentation.

## Structure

```text
components/
|-- application/
|   |-- app_network_coordinator/
|   `-- app_reset_coordinator/
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
|-- sensing/
|   |-- sensor_manager/
|   `-- sensor_DHT22/
|-- storage/
|   |-- config_manager/
|   `-- sd_card_manager/
|-- system/
|   |-- common/
|   `-- performance_monitor/
`-- ui/
    |-- app_gui/
    |-- lvgl_image_handler/
    |-- lvgl_sd_fs/
    `-- ui_manager_lvgl/
```

The project-level `CMakeLists.txt` registers these group directories through
`EXTRA_COMPONENT_DIRS`. ESP-IDF discovers each child component. Group folders do
not define facade APIs or runtime behavior.

## Ownership

| Domain | Components | Primary responsibility |
|---|---|---|
| Application | `app_network_coordinator`, `app_reset_coordinator` | Network boot policy and ordered factory reset |
| Cloud | `cloud_manager`, `firebase_auth` | Authenticated Firebase telemetry and token lifecycle |
| Connectivity | `provisioning_manager`, `wifi_manager` | BLE provisioning and Wi-Fi Station lifecycle |
| Display | `display_driver`, `waveshare__esp_lcd_st7735` | LCD bus and panel integration |
| Input | `button_manager` | Debounced button events |
| Sensing | `sensor_manager`, `sensor_DHT22` | Sensor policy and DHT22 acquisition |
| Storage | `config_manager`, `sd_card_manager` | Persistent configuration and SD access |
| System | `common`, `performance_monitor` | Shared definitions and diagnostics |
| UI | `app_gui`, `ui_manager_lvgl`, `lvgl_image_handler`, `lvgl_sd_fs` | Screens, LVGL ownership, and media support |

## Dependency Rules

- Application code uses public component APIs.
- High-level components declare lower-level driver dependencies through CMake.
- Domain folders introduce no task, queue, mutex, state machine, or lifecycle.
- `managed_components/` remains owned by ESP-IDF Component Manager.
- Cross-domain facade APIs require a separate design decision.

## Documentation Index

- Application composition: [`main/README.md`](../main/README.md)
- System architecture: [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md)
- Build and hardware setup: [`docs/SETUP.md`](../docs/SETUP.md)
- Firebase Authentication component:
  [`cloud/firebase_auth/docs/README.md`](cloud/firebase_auth/docs/README.md)
- Firebase setup and security:
  [`cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md`](cloud/firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)
- Cloud telemetry component:
  [`cloud/cloud_manager/docs/README.md`](cloud/cloud_manager/docs/README.md)

Other component-specific behavior and limitations remain in each component's
`docs/README.md`.
