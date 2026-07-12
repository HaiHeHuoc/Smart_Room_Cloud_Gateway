# app_gui Component Notes

## Purpose

`app_gui` owns application-facing LVGL tasks, screens, and UI data queues.
`ui_manager_lvgl` remains responsible for LVGL/display initialization, mutex,
tick timer, and LCD flush integration.

## What Is Implemented

- Owns the 24 KB application GUI task that calls `lv_timer_handler()` every
  33 ms.
- Locks the LVGL mutex while processing queued UI data and LVGL timers.
- Logs GUI task stack high-water status every 60 seconds.
- Owns a length-one Wi-Fi status queue that always keeps the newest update.
- Defines `ui_wifi_state_t` and `ui_wifi_status_t`.
- Provides a non-blocking Wi-Fi status post API suitable for the ESP event-loop
  callback path.
- Provides the existing optional counter/alignment demo task.

## Initialization Order

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(app_gui_init());
ESP_ERROR_CHECK(app_gui_start_ui_task());
```

The main GUI task must run continuously so LVGL timers, rendering, animations,
and queued Wi-Fi status updates continue to progress.

## Public API

| API | Role |
| --- | --- |
| `app_gui_init()` | Create GUI-owned resources, including the Wi-Fi status queue. |
| `app_gui_start_ui_task()` | Start the main LVGL/application GUI task. |
| `app_gui_post_wifi_status()` | Overwrite the queue with the newest Wi-Fi status without waiting. |
| `app_gui_create_demo_screen()` | Create the static `LVGL OK` demo screen. |
| `app_gui_start_running_demo_task()` | Start the optional moving counter demo task. |

## Wi-Fi Update Flow

```text
wifi_manager event
    -> application callback maps status to ui_wifi_status_t
    -> app_gui_post_wifi_status()
    -> app_gui UI task receives the newest status
    -> future Wi-Fi widgets are updated while the LVGL mutex is held
```

The current implementation logs received Wi-Fi data. It does not yet bind the
data to dedicated status widgets.

## Important Notes

- `app_gui_init()` must run after `ui_manager_lvgl_init()` and before either
  task-start API.
- The queue length is one by design. Intermediate Wi-Fi transitions may be
  replaced if producers are faster than the GUI task; the latest state wins.
- `app_gui_post_wifi_status()` does not call LVGL and does not block.
- Only the main app GUI task should call `lv_timer_handler()`.
- The optional demo task accesses LVGL under the UI manager mutex.
- There is no deinit/stop API or duplicate-task guard yet.

## Future Attention

- Replace Wi-Fi status logging with real labels/icons.
- Replace the moving counter demo with the Phase 2 dashboard.
- Track task handles to prevent duplicate task starts.
- Add deinit only when application shutdown/restart is required.
