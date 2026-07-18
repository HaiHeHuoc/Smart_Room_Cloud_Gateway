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
- Tracks the active application screen as `NONE`, `WIFI`, or `SENSOR` using
  thread-safe set/get APIs.
- Provides a non-blocking Wi-Fi status post API suitable for the ESP event-loop
  callback path.
- Displays Wi-Fi mode, SSID, and IPv4 address on a fixed 160 x 128 landscape
  screen.
- Displays DHT22 temperature, humidity, and sensor state on a matching sensor
  screen. Stale data remains visible and is marked as `STALE`.
- Uses state-specific colors and ellipsis for values that exceed the available
  row width.
- Provides the existing optional counter/alignment demo task.

## Initialization Order

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(app_gui_init());
ESP_ERROR_CHECK(app_gui_start_ui_task());
ESP_ERROR_CHECK(app_gui_create_wifi_screen());
```

The main GUI task must run continuously so LVGL timers, rendering, animations,
and queued Wi-Fi status updates continue to progress.

## Public API

| API | Role |
| --- | --- |
| `app_gui_init()` | Create the Wi-Fi status queue. |
| `app_gui_start_ui_task()` | Start the main LVGL/application GUI task. |
| `app_gui_create_wifi_screen()` | Create the Wi-Fi status widgets on the active screen while holding the LVGL mutex. |
| `app_gui_create_sensor_screen()` | Create the sensor status widgets and mark the Sensor screen active. |
| `app_gui_post_wifi_status()` | Overwrite the queue with the newest Wi-Fi status without waiting. |
| `app_gui_post_sensor_status()` | Queue a sensor status snapshot without waiting. |
| `app_gui_set_screen_id()` | Store the active application screen ID without changing LVGL objects. |
| `app_gui_get_screen_id()` | Read a thread-safe snapshot of the active application screen ID. |
| `app_gui_create_demo_screen()` | Create the static `LVGL OK` demo screen. |
| `app_gui_start_running_demo_task()` | Start the optional moving counter demo task. |

## Wi-Fi Update Flow

```text
wifi_manager event
    -> application callback maps status to ui_wifi_status_t
    -> app_gui_post_wifi_status()
    -> app_gui UI task receives the newest status
    -> Mode, SSID, and IP labels update while the LVGL mutex is held
```

The screen maps `IDLE`, `CONNECTING`, `WAITING IP`, `CONNECTED`,
`DISCONNECTED`, and `FAILED` to readable mode text. The IP row shows `-` until
an IPv4 address is valid.

## Sensor Update Flow

```text
sensor_manager callback
    -> app_gui_post_sensor_status()
    -> app_gui UI task receives the sensor snapshot
    -> Sensor screen updates temperature, humidity, and state
```

Sensor updates do not replace an active Wi-Fi screen. After the Wi-Fi timeout
clears that screen to `NONE`, the next sensor snapshot creates and updates the
Sensor screen. A new Wi-Fi event can bring the Wi-Fi screen to the foreground
again.

## Important Notes

- `app_gui_init()` must run after `ui_manager_lvgl_init()` and before either
  task-start API.
- The queue length is one by design. Intermediate Wi-Fi transitions may be
  replaced if producers are faster than the GUI task; the latest state wins.
- `app_gui_post_wifi_status()` does not call LVGL and does not block.
- `app_gui_create_wifi_screen()` releases the LVGL mutex on success and on all
  current error paths.
- Screen ID APIs protect only the ID value. LVGL screen creation, loading, and
  deletion must still run in the GUI task or while holding the LVGL mutex.
- Only the main app GUI task should call `lv_timer_handler()`.
- The optional demo task accesses LVGL under the UI manager mutex.
- There is no deinit/stop API or duplicate-task guard yet.

## Future Attention

- Add RSSI and a compact signal-strength indicator if required.
- Replace or remove the moving counter demo after Phase 2 validation.
- Track task handles to prevent duplicate task starts.
- Add deinit only when application shutdown/restart is required.
