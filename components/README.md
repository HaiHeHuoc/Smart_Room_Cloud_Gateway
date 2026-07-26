# Component Organization

This directory groups reusable ESP-IDF components by technical domain. The
group directories are organizational containers; each child directory remains
an independent ESP-IDF component with its own name, public API, dependencies,
and documentation.

## Structure

```text
components/
|-- application/
|   `-- app_network_coordinator/
|-- cloud/
|   |-- cloud_manager/
|   `-- firebase_auth/
|-- connectivity/
|   |-- provisioning_manager/
|   `-- wifi_manager/
|-- display/
|   |-- display_driver/
|   `-- waveshare__esp_lcd_st7735/
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
`EXTRA_COMPONENT_DIRS`. ESP-IDF then discovers each child component. The group
directories intentionally do not define facade APIs or their own component
targets.

## Ownership

| Domain | Components | Primary responsibility |
|---|---|---|
| Application | `app_network_coordinator` | Network boot policy and manager coordination |
| Cloud | `cloud_manager`, `firebase_auth` | Authenticated Firebase telemetry |
| Connectivity | `provisioning_manager`, `wifi_manager` | BLE provisioning and Wi-Fi connection lifecycle |
| Display | `display_driver`, `waveshare__esp_lcd_st7735` | LCD bus and panel integration |
| Sensing | `sensor_manager`, `sensor_DHT22` | Sensor policy and DHT22 acquisition |
| Storage | `config_manager`, `sd_card_manager` | Persistent configuration and SD-card access |
| System | `common`, `performance_monitor` | Shared definitions and diagnostics |
| UI | `app_gui`, `ui_manager_lvgl`, `lvgl_image_handler`, `lvgl_sd_fs` | Screens, LVGL ownership, and media support |

## Dependency Rules

- Application code uses each component's existing public API; moving a
  component does not merge or hide that API.
- High-level components depend on lower-level drivers through their declared
  CMake requirements.
- Domain folders are not runtime owners and do not introduce initialization,
  task, mutex, or event behavior.
- `managed_components/` remains managed by ESP-IDF and is not part of this
  organization.
- New components or cross-domain facade APIs require separate design approval.

## Documentation

Component-specific usage, behavior, and limitations remain in each component's
`docs/README.md` or existing component README. The application startup contract
is documented in `main/README.md`.
