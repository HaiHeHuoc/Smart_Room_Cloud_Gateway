# app_gui

## Purpose

`app_gui` owns application screen models, asynchronous screen routing, LVGL
objects, and the single UI task that services `lv_timer_handler()`.
`ui_manager_lvgl` continues to own LVGL initialization, display integration,
the tick timer, and the non-recursive LVGL mutex.

Phase 6.4.1 centralizes all production screen creation, rendering, cleanup, and
transitions in the `app_gui` UI task. Producers and application-policy
components may only post screen commands or copied status snapshots.

## Application Screens

| Screen ID | Phase 6.4.1 role |
|---|---|
| `APP_GUI_SCREEN_NONE` | Internal state before the first successful route; never a public request target. |
| `APP_GUI_SCREEN_BOOT` | Static `Smart Gateway` / `Starting...` placeholder. |
| `APP_GUI_SCREEN_PROVISIONING` | Static `Wi-Fi Setup` / `Preparing setup...` placeholder. |
| `APP_GUI_SCREEN_WIFI_STATUS` | Existing Wi-Fi mode, SSID, and IPv4 screen. |
| `APP_GUI_SCREEN_SENSOR_DASHBOARD` | Existing sensor dashboard with Wi-Fi and cloud summaries. |

The old `APP_GUI_SCREEN_WIFI` and `APP_GUI_SCREEN_SENSOR` identifiers were
renamed directly. No compatibility aliases are retained.

## Queues And Public API

`app_gui_init()` creates all queues as one initialization transaction:

- Command queue: length 8, element type `app_gui_command_t`.
- Wi-Fi status queue: length 1, newest snapshot overwrites the pending value.
- Sensor status queue: length 5, producers never wait.
- Cloud status queue: length 1, newest snapshot overwrites the pending value.

If any allocation fails, every queue created by that attempt is deleted, all
handles return to `NULL`, and `app_gui_init()` returns `ESP_ERR_NO_MEM`.

| API | Responsibility |
|---|---|
| `app_gui_init()` | Create command and status queues. |
| `app_gui_start_ui_task()` | Start the 24 KB, priority-5 UI task. |
| `app_gui_request_screen()` | Validate and enqueue a non-blocking asynchronous screen request. |
| `app_gui_get_screen_id()` | Copy the authoritative active-screen ID under a critical section. |
| `app_gui_post_wifi_status()` | Replace the pending Wi-Fi model without calling LVGL. |
| `app_gui_post_sensor_status()` | Queue a sensor model without calling LVGL. |
| `app_gui_post_cloud_status()` | Replace the pending cloud model without calling LVGL. |

`app_gui_request_screen()` is safe from normal task and task-context callback
code. It does not wait for construction and is not ISR-safe. `ESP_OK` means
only that the command was queued. It returns `ESP_ERR_TIMEOUT` when the command
queue is full.

## UI Task And Router

Every 33 ms the UI task performs:

```text
drain screen commands in FIFO order
-> consume/cache/render status queues
-> take LVGL mutex
-> lv_timer_handler()
-> release LVGL mutex
```

`app_gui_activate_screen()` is the only normal-runtime writer of the
authoritative active-screen ID. For each non-duplicate request it:

1. Keeps the current root loaded.
2. Creates and builds a separate target root.
3. Preserves the current root and widget references if target construction or
   Wi-Fi timer allocation fails.
4. Loads the target only after successful construction.
5. Updates the active-screen ID only after the target is valid.
6. Pauses timers that belong to inactive screens.
7. Replaces every widget reference with the new screen's references.
8. Renders the latest cached model for the target.
9. Deletes the previous inactive root.

A request for the already-active screen is accepted but does not rebuild the
screen or restart its timer. Cached data may be rendered again.

## LVGL Mutex Contract

The router uses contract A:

```text
app_gui UI task
    -> acquire non-recursive LVGL mutex
    -> call private no-lock screen builders and renderers
    -> release LVGL mutex
```

Status handlers acquire the mutex only when rendering a model on its active
screen. Producers, coordinator code, and ESP/Wi-Fi/provisioning/cloud/sensor
callbacks never call LVGL.

The Wi-Fi timeout callback already runs inside `lv_timer_handler()` while the
mutex is held. It therefore only posts a non-blocking
`APP_GUI_SCREEN_SENSOR_DASHBOARD` command and pauses the timer. The command is
processed during the next UI iteration; the callback never tries to take the
mutex recursively.

## Model Caching

The GUI retains the latest complete:

- `ui_wifi_status_t` plus an availability flag;
- `ui_sensor_status_t` plus an availability flag;
- `ui_cloud_status_t` plus an availability flag.

Status events never choose a screen:

- Wi-Fi updates render only on `WIFI_STATUS` or update the Wi-Fi summary on
  `SENSOR_DASHBOARD`.
- Sensor updates render only on `SENSOR_DASHBOARD`.
- Cloud updates render only on `SENSOR_DASHBOARD`.
- Updates received for inactive screens only refresh cached models and never
  touch deleted LVGL pointers.

Entering `WIFI_STATUS` renders the latest Wi-Fi model. Entering
`SENSOR_DASHBOARD` renders the latest sensor, Wi-Fi summary, and cloud models.

## Application Routing

The network coordinator owns config-driven and normal-network routing policy.
It posts screen requests but never renders or calls LVGL.

| Trigger | Route or behavior |
|---|---|
| Config `VALID` after verification/migration | `BOOT` -> `WIFI_STATUS` after verified normal `GOT_IP` -> `SENSOR_DASHBOARD` after the existing timeout |
| Config `NOT_CONFIGURED` | `PROVISIONING` placeholder directly |
| Config migration | Resolve migration, then use `BOOT` for the final valid path |
| Config inspection or unrecoverable integrity error | Best-effort `BOOT` placeholder plus safe error logging |
| Wi-Fi status update | Cache/render only; no implicit screen change |
| Sensor status update | Cache/render only; no implicit screen change |
| Cloud status update | Cache/render only; no implicit screen change |
| Wi-Fi screen timeout | Explicit deferred `SENSOR_DASHBOARD` request |

The coordinator ignores transient provisioning Wi-Fi events while it remains
in `PROVISIONING`. Only a normal runtime transition from `CONNECTING` or
`OFFLINE` to `ONLINE` queues `WIFI_STATUS`. Successful provisioning-to-screen
state integration remains deferred to Phase 6.4.4.

## Error Handling

- Invalid or `NONE` screen requests return `ESP_ERR_INVALID_ARG`.
- Requests before `app_gui_init()` return `ESP_ERR_INVALID_STATE`.
- A full command queue returns `ESP_ERR_TIMEOUT`.
- Missing LVGL roots return `ESP_ERR_INVALID_STATE`.
- Widget or timer allocation failures return `ESP_ERR_NO_MEM`.
- Target construction failure is logged by the UI task and leaves the
  previously visible screen and active-screen ID unchanged.
- Screen failures do not reboot the device or change network policy.

## Initialization Order

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(app_gui_init());
ESP_ERROR_CHECK(app_gui_start_ui_task());
ESP_ERROR_CHECK(app_network_coordinator_init(&network_config));
ESP_ERROR_CHECK(app_network_coordinator_start());
```

`main` does not create an initial screen directly. The coordinator requests the
initial screen only after resolving the final configuration state.

## Phase 6.4.1 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

Phase 6.4.1 intentionally does not implement:

- QR code or `lv_qrcode`;
- provisioning payload, service name, PoP, or security changes;
- provisioning progress/retry/timeout UI;
- successful provisioning-to-`WIFI_STATUS` routing;
- factory reset, touch navigation, animation, progress bar, or dashboard
  redesign;
- BLE lifecycle, Wi-Fi retry, Firebase, or cloud-task changes.

Phase 6.4 is not complete.
