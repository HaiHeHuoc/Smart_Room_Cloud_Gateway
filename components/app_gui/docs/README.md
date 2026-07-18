# app_gui Component Notes

## Purpose

`app_gui` owns application-facing LVGL screens, the GUI task, and the queues
used to move Wi-Fi and sensor snapshots into the LVGL context.
`ui_manager_lvgl` continues to own LVGL initialization, the tick timer, display
flush integration, and the non-recursive LVGL mutex.

## Source Organization

The implementation remains in one source file to keep the existing state and
call sequence unchanged. `app_gui.c` is grouped into these responsibilities:

- GUI core: queues, active screen ID, GUI task, stack monitoring, and LVGL
  timer handling.
- Wi-Fi UI: Wi-Fi widgets, state formatting, update processing, and the
  reusable 10-second screen timer.
- Sensor UI: temperature/humidity widgets, state formatting, and sensor queue
  processing.
- Demo UI: optional static and moving-label demonstrations.

## What Is Implemented

- A 24 KB GUI task that runs every 33 ms and is the only project task that
  calls `lv_timer_handler()`.
- A length-one Wi-Fi queue updated with `xQueueOverwrite()`, so only the newest
  pending Wi-Fi snapshot is retained.
- A length-five sensor queue updated without waiting; a full queue causes that
  sample to be dropped.
- Thread-safe tracking of `NONE`, `WIFI`, and `SENSOR` screen IDs.
- A Wi-Fi screen showing mode, SSID, and IPv4 address.
- A sensor screen showing temperature, humidity, and data state.
- A reusable LVGL timer that clears the Wi-Fi screen after 10 seconds without
  a new Wi-Fi event.
- Stack high-water logging every 60 seconds.

## Initialization Order

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(app_gui_init());
ESP_ERROR_CHECK(app_gui_start_ui_task());
ESP_ERROR_CHECK(app_gui_create_wifi_screen());
```

The GUI task must keep running for queued updates, LVGL timers, rendering, and
animations to progress.

## Public API

| API | Current role |
| --- | --- |
| `app_gui_init()` | Create the Wi-Fi and sensor status queues. |
| `app_gui_start_ui_task()` | Start the application GUI/LVGL timer task. |
| `app_gui_create_wifi_screen()` | Build and activate Wi-Fi widgets while internally holding the LVGL mutex. |
| `app_gui_post_wifi_status()` | Replace the pending Wi-Fi snapshot without waiting. |
| `app_gui_create_sensor_screen()` | Build and activate sensor widgets while internally holding the LVGL mutex. |
| `app_gui_post_sensor_status()` | Append a sensor snapshot without waiting. |
| `app_gui_set_screen_id()` | Update only the tracked screen ID. |
| `app_gui_get_screen_id()` | Read the tracked screen ID under a critical section. |
| `app_gui_clear_screen()` | Replace the active LVGL root screen; the caller must serialize LVGL access. |
| `app_gui_create_demo_screen()` | Create the static `LVGL OK` demo. |
| `app_gui_start_running_demo_task()` | Start the optional moving-label demo task. |

## Wi-Fi Flow

```text
wifi_manager event
    -> main callback maps wifi_manager_status_t to ui_wifi_status_t
    -> app_gui_post_wifi_status()
    -> GUI task receives the newest pending snapshot
    -> Wi-Fi screen is created if necessary
    -> 10-second timer is restarted
    -> Mode, SSID, and IP labels are updated
```

The timer callback runs from `lv_timer_handler()` while the GUI task owns the
LVGL mutex. It clears only an active Wi-Fi screen, pauses itself, and leaves the
tracked screen ID as `NONE`.

## Sensor Flow

```text
sensor_manager task
    -> main callback maps sensor_manager_status_t to ui_sensor_status_t
    -> app_gui_post_sensor_status()
    -> GUI task receives one pending snapshot
    -> Sensor screen is created only when the active ID is NONE
    -> Temperature, humidity, and state labels are updated
```

An active Wi-Fi screen has priority over sensor snapshots. After the Wi-Fi
timeout clears that screen, a later sensor snapshot can create the sensor
screen. A subsequent Wi-Fi event brings the Wi-Fi screen forward again.
When sensor data is stale, the temperature and humidity labels display `-`
instead of retaining the last-known-good values.

## Thread-Safety Contract

- Status post APIs never call LVGL and return without waiting.
- Screen creation, internal label updates, and queue-driven screen/timer
  transitions acquire the LVGL mutex.
- `app_gui_clear_screen()` deliberately does not acquire the mutex. Call it
  only while already holding the mutex, from the mutex-protected LVGL timer
  callback path, or from another path that exclusively owns LVGL access.
- The LVGL mutex is non-recursive. A timer callback running inside
  `lv_timer_handler()` must not lock it again.
- Screen ID locking protects only the enum value, not LVGL objects.
- Callback producers must copy status through the queue and must not call LVGL
  directly.

## Current Limitations

- There is no stop/deinit API or duplicate-task guard.
- The sensor queue processes one queued snapshot per GUI iteration rather than
  collapsing directly to the newest snapshot.
- RSSI and disconnect reason are transported but not displayed.
- `app_gui_clear_screen()` relies on its caller to satisfy the LVGL ownership
  contract.
- The optional demo task is retained for development only.

## Future Attention

- Centralize every screen transition inside one explicit GUI command queue.
- Add a stop/deinit path only when runtime shutdown is required.
- Decide whether sensor updates should preserve history or overwrite older
  pending snapshots.
- Add RSSI presentation when it is useful to the product UI.
