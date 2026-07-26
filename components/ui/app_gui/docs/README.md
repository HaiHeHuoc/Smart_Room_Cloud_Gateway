# app_gui Component Notes

## Purpose

`app_gui` owns application-facing LVGL screens, the GUI task, and the queues
used to move Wi-Fi, sensor, and cloud snapshots into the LVGL context.
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
- Cloud UI: cloud state formatting, retained dashboard status, and cloud queue
  processing.
- Demo UI: optional static and moving-label demonstrations.

## What Is Implemented

- A 24 KB GUI task that runs every 33 ms and is the only project task that
  calls `lv_timer_handler()`.
- A length-one Wi-Fi queue updated with `xQueueOverwrite()`, so only the newest
  pending Wi-Fi snapshot is retained.
- A length-five sensor queue updated without waiting; a full queue causes that
  sample to be dropped.
- A length-one cloud queue updated with `xQueueOverwrite()`, so transient cloud
  state changes cannot block the cloud task.
- Thread-safe tracking of `NONE`, `WIFI`, and `SENSOR` screen IDs.
- A Wi-Fi screen showing mode, SSID, and IPv4 address.
- A compact 160x128 Smart Room dashboard with a framed header, two-column
  layout, temperature/humidity readings, Wi-Fi summary, cloud status, and
  sensor health.
- A reusable LVGL timer that requests the Sensor screen after 10 seconds
  without a new Wi-Fi event.
- In-place screen rebuilding on one active LVGL root, avoiding an intermediate
  default-white screen and a second-root allocation peak.
- Retention of the latest sensor snapshot so the dashboard can be populated
  immediately when the Wi-Fi screen times out.
- Stack high-water logging every 60 seconds.
- Guards against starting a second GUI task or a second demo task.

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
| `app_gui_init()` | Create the Wi-Fi, sensor, and cloud status queues. |
| `app_gui_start_ui_task()` | Start the application GUI/LVGL timer task. |
| `app_gui_create_wifi_screen()` | Rebuild the active root with Wi-Fi widgets while internally holding the LVGL mutex. |
| `app_gui_post_wifi_status()` | Replace the pending Wi-Fi snapshot without waiting. |
| `app_gui_create_sensor_screen()` | Rebuild the active root with sensor widgets while internally holding the LVGL mutex. |
| `app_gui_post_sensor_status()` | Append a sensor snapshot without waiting. |
| `app_gui_post_cloud_status()` | Replace the pending cloud snapshot without waiting. |
| `app_gui_set_screen_id()` | Update only the tracked screen ID. |
| `app_gui_get_screen_id()` | Read the tracked screen ID under a critical section. |
| `app_gui_clear_screen()` | Clear the active root in place; the caller must serialize LVGL access. |
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
non-recursive LVGL mutex. It records a Sensor-screen request and pauses itself
without deleting the visible Wi-Fi widgets. On the next GUI iteration, the
request is handled before rendering and the active root is rebuilt directly as
the Sensor screen.

## Sensor Flow

```text
sensor_manager task
    -> main callback maps sensor_manager_status_t to ui_sensor_status_t
    -> app_gui_post_sensor_status()
    -> GUI task receives one pending snapshot
    -> latest sensor snapshot is retained
    -> Sensor screen is created when the active ID is NONE or Wi-Fi times out
    -> Temperature, humidity, and state labels are updated
```

An active Wi-Fi screen has priority over sensor snapshots until its timer
expires. The current Wi-Fi screen remains visible until the GUI task can
rebuild that same root as the Sensor screen. If a sensor snapshot is available,
its values are restored during the same GUI iteration; otherwise the Sensor
screen uses placeholders until the first sample arrives. A subsequent Wi-Fi
event cancels a pending Sensor transition and rebuilds the same root as the
Wi-Fi screen.
When sensor data is invalid or stale, the temperature and humidity labels
display `-`. The same placeholder is displayed immediately when
`sensor_manager` posts its `-1.0f` failed-read sentinel, even during the
temporary `DEGRADED` window before the stale timeout expires.

The sensor dashboard is arranged as:

```text
Smart Room                         WiFi [dot] C [dot]
----------------------------------------------------
temperature        | Wi-Fi: Online/Offline/--
                   | Cloud: Wait/Sync/Online/Retry/Auth/Error
humidity           | Sensor: OK/Warn/Error/Stale
```

The Wi-Fi row and header dot use the latest status consumed by the GUI task.
The Cloud row and header dot use the latest cloud-manager status consumed by
the GUI task. That retained summary is restored whenever the Sensor screen is
created again.

## Cloud Flow

```text
cloud_manager state or upload result changes
    -> main callback maps cloud_manager_status_t to ui_cloud_status_t
    -> app_gui_post_cloud_status()
    -> GUI task receives the newest pending snapshot
    -> Cloud row and header dot are updated on the Sensor screen
```

Displayed states are `Wait`, `Sync`, `Online`, `Retry`, `Auth`, and `Error`.
The Cloud indicator remains `--` only until the first manager status reaches
the GUI queue.

## Thread-Safety Contract

- Status post APIs never call LVGL and return without waiting.
- Screen creation, internal label updates, and queue-driven screen/timer
  transitions acquire the LVGL mutex.
- `app_gui_clear_screen()` deliberately does not acquire the mutex. Call it
  only while already holding the mutex, from the mutex-protected LVGL timer
  callback path, or from another path that exclusively owns LVGL access.
- Wi-Fi timeout handling records only a small deferred-transition flag while
  inside `lv_timer_handler()`; it never attempts to take the LVGL mutex again.
- The LVGL mutex is non-recursive. A timer callback running inside
  `lv_timer_handler()` must not lock it again.
- The screen-state critical section protects the screen ID, retained status
  summaries, and the deferred Sensor-screen request, but not LVGL objects.
- Callback producers must copy status through the queue and must not call LVGL
  directly.

## Current Limitations

- There is no stop/deinit API. Task handles are retained only to prevent
  duplicate task creation.
- The sensor queue processes one queued snapshot per GUI iteration rather than
  collapsing directly to the newest snapshot.
- RSSI and disconnect reason are transported but not displayed.
- Failed sensor readings currently depend on the private `-1.0f` producer/UI
  convention in addition to `data_valid` and `data_stale`.
- `app_gui_clear_screen()` reuses the active root but still relies on its
  caller to satisfy the LVGL ownership contract.
- The optional demo task is retained for development only. It stops itself if
  another screen transition deletes its label.

## Future Attention

- Add an explicit GUI command queue only when more asynchronous screen types
  require centralized transition policy.
- Add a stop/deinit path only when runtime shutdown is required.
- Decide whether sensor updates should preserve history or overwrite older
  pending snapshots.
- Add RSSI presentation when it is useful to the product UI.
