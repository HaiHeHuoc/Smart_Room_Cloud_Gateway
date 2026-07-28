# app_gui

## Purpose

`app_gui` owns application screen models, asynchronous screen routing, LVGL
objects, and the single UI task that services `lv_timer_handler()`.
`ui_manager_lvgl` continues to own LVGL initialization, display integration,
the tick timer, and the non-recursive LVGL mutex.

Phase 6.4.1 centralizes all production screen creation, rendering, cleanup, and
transitions in the `app_gui` UI task. Producers and application-policy
components may only post screen commands or copied status snapshots.
Phase 6.4.2 adds the provisioning-screen visual foundation, UI-only state
model, copied status command, cache, and stable renderer without connecting
them to the real BLE provisioning lifecycle.
Phase 6.4.3 replaces the reserved region with the official LVGL QR widget,
adds a dedicated copied-payload queue, and renders the active Espressif
Security 1 provisioning payload.
Phase 6.4.4 connects the model to real manager/coordinator/Wi-Fi progress and
adds verified success routing.
Phase 6.4.5 makes provisioning status, QR updates, and QR invalidation
generation-aware so delayed messages from an old session cannot corrupt the
new session's screen model.

## Application Screens

| Screen ID | Phase 6.4.1 role |
|---|---|
| `APP_GUI_SCREEN_NONE` | Internal state before the first successful route; never a public request target. |
| `APP_GUI_SCREEN_BOOT` | Static `Smart Gateway` / `Starting...` placeholder. |
| `APP_GUI_SCREEN_PROVISIONING` | Stable provisioning layout with a scannable QR code, instruction/status labels, and state indicator. |
| `APP_GUI_SCREEN_WIFI_STATUS` | Existing Wi-Fi mode, SSID, and IPv4 screen. |
| `APP_GUI_SCREEN_SENSOR_DASHBOARD` | Existing sensor dashboard with Wi-Fi and cloud summaries. |

The old `APP_GUI_SCREEN_WIFI` and `APP_GUI_SCREEN_SENSOR` identifiers were
renamed directly. No compatibility aliases are retained.

## Queues And Public API

`app_gui_init()` creates all queues as one initialization transaction:

- Command queue: length 8, carrying copied screen requests only.
- Provisioning status queue: length 1, newest complete snapshot overwrites the
  pending value.
- Provisioning QR queue: length 1, newest payload-or-unavailable message
  overwrites the pending value.
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
| `app_gui_post_provisioning_status()` | Validate and overwrite the pending non-sensitive provisioning model. |
| `app_gui_post_provisioning_qr_payload()` | Validate and copy the active Security 1 QR payload into the dedicated latest-value queue. |
| `app_gui_clear_provisioning_qr_payload()` | Post explicit session invalidation; the GUI task clears its cache and hides the QR widget. |
| `app_gui_post_wifi_status()` | Replace the pending Wi-Fi model without calling LVGL. |
| `app_gui_post_sensor_status()` | Queue a sensor model without calling LVGL. |
| `app_gui_post_cloud_status()` | Replace the pending cloud model without calling LVGL. |

`app_gui_request_screen()` is safe from normal task and task-context callback
code. It does not wait for construction and is not ISR-safe. `ESP_OK` means
only that the command was queued. It returns `ESP_ERR_TIMEOUT` when the command
queue is full.

`app_gui_post_provisioning_status()` uses `xQueueOverwrite()` on its dedicated
length-one queue. Rapid progress can coalesce, but an older state can never
fill the screen-command queue or displace a screen transition. It never
retains the caller's pointer, calls LVGL, or activates a screen. The payload
contains only a non-zero session generation, UI state, `esp_err_t`, and a
disconnect reason; it contains no SSID, password, PoP, token, raw framework
object, or manager-owned pointer.

`app_gui_post_provisioning_qr_payload()` also never retains the caller's
pointer, calls LVGL, or changes screens. Its copied payload is sensitive
because it contains the development PoP and must not be logged.
`app_gui_clear_provisioning_qr_payload()` follows the same producer contract;
the caller supplies the non-zero session generation and only the GUI task
clears cached QR bytes or touches the QR widget.

## UI Task And Router

Every 33 ms the UI task performs:

```text
drain screen commands in FIFO order
-> consume/cache/render provisioning QR
-> consume/cache/render latest provisioning status
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

- `ui_provisioning_status_t` plus an availability flag;
- `ui_provisioning_qr_payload_t` plus an availability flag;
- `ui_wifi_status_t` plus an availability flag;
- `ui_sensor_status_t` plus an availability flag;
- `ui_cloud_status_t` plus an availability flag.

It also retains the newest accepted provisioning generation. A strictly newer
generation clears both old provisioning caches before its first status or QR
message is applied. Status, QR update, and QR clear messages with an older
generation are rejected at DEBUG level both before they can overwrite a
pending newer queue value and again in the GUI consumer. Generation zero is
invalid; numeric wrap skips zero, while generic long-distance wrap ordering is
intentionally outside this bounded device-lifetime policy.

Status events never choose a screen:

- Provisioning updates render only on `PROVISIONING`.
- Wi-Fi updates render only on `WIFI_STATUS` or update the Wi-Fi summary on
  `SENSOR_DASHBOARD`.
- Sensor updates render only on `SENSOR_DASHBOARD`.
- Cloud updates render only on `SENSOR_DASHBOARD`.
- Updates received for inactive screens only refresh cached models and never
  touch deleted LVGL pointers.

Entering `PROVISIONING` renders the latest provisioning model, or the default
`STARTING` model before the first update, and renders the latest valid QR
payload when available. Entering `WIFI_STATUS` renders the latest Wi-Fi model.
Entering
`SENSOR_DASHBOARD` renders the latest sensor, Wi-Fi summary, and cloud models.

## Provisioning UI And QR

### UI-only model

`ui_provisioning_status_t` carries:

- `uint32_t session_generation` (non-zero);
- `uint32_t session_number` (one-based active session);
- `uint32_t session_limit` (configured maximum, including the first session);
- `ui_provisioning_state_t state`;
- `esp_err_t last_error`;
- `uint16_t wifi_disconnect_reason`.

Supported states are `STARTING`, `WAITING_FOR_PHONE`,
`CREDENTIAL_RECEIVED`, `CONNECTING_WIFI`, `WAITING_FOR_IP`,
`SAVING_CONFIG`, `CLEANING_UP`, `SUCCESS`, `FAILED`, `TIMEOUT`, and
`RETRYING`.

The default cached model is:

```text
state = STARTING
last_error = ESP_OK
wifi_disconnect_reason = 0
```

Current QR flow:

```text
active provisioning_manager service
    -> coordinator copies exact QR JSON
    -> app_gui_post_provisioning_qr_payload()
    -> dedicated length-one GUI queue
    -> app_gui UI task
    -> lv_qrcode_update()
```

`ui_provisioning_qr_payload_t` and explicit QR-clear messages carry the same
generation. This two-layer ordering guard prevents an old `STOPPED`/`FAILED`
clear from hiding the new QR and prevents an old terminal status from
overwriting a new `WAITING_FOR_PHONE` state.

The QR cache belongs to the provisioning session, not to one screen instance.
Leaving `PROVISIONING` does not clear it. A coordinator/manager terminal stop,
failure, timeout, or successful handoff posts an unavailable message; the GUI
task then zeroizes the cache and hides the QR object when that screen is
active. Ordinary credential failure keeps the same QR available so the phone
can submit another credential set in the same BLE session.

Real progress flow:

```text
provisioning_manager copied progress
    + existing wifi_manager callback bridge
    + coordinator persistence/cleanup/adoption policy
    -> ui_provisioning_status_t
    -> dedicated length-one status queue
    -> app_gui UI task
    -> text/color render only when PROVISIONING is active
```

### 160x128 layout

```text
+--------------------------------------+
| Wi-Fi Setup                       [o] |
|--------------------------------------|
| +---------------------+ Session 1/3  |
| |                     |              |
| |  74x74 QR symbol    | instruction  |
| |  in white 92x92     |              |
| |  quiet-zone holder  | status       |
| +---------------------+              |
+--------------------------------------+
```

The white container occupies `(0, 36)` through `(91, 127)`. The QR symbol is
`74 x 74` and centered, leaving a 9-pixel white quiet zone on every side.
The current 73-byte Security 1 payload is QR version 5 at LVGL medium error
correction: 37 modules scaled by an exact 2 pixels per module. This is the
largest integer module scale that fits the 160x128 layout while preserving at
least the required four-module quiet zone. Disabling the widget's internal
quiet zone prevents it from shrinking the symbol; the measured white parent
provides that zone instead.

The right column begins at `x=94`; all objects remain within the 160x128
logical display. The instruction label prefixes its existing state text with
`Session n/max`; no new LVGL object is created and QR size/quiet zone are
unchanged. The QR object is created once with the screen and stays hidden until
a valid payload has been encoded. The current QR canvas consumes roughly 0.9
KiB plus LVGL object metadata. Encoding version 5 temporarily allocates two
173-byte work buffers. Application-side payload storage is bounded to 192
bytes per manager/cache/queue copy, requires no PSRAM, and is never persisted
to NVS.

GUI task-local QR copies are securely overwritten after rendering. The cached
copy is securely cleared only by an explicit session-unavailable message. The
manager independently clears its active copy when BLE stop begins.

The title, instruction, status, and state indicator are created once when the
screen is activated. Status updates modify only their text and colors. There
is no provisioning-specific timer, spinner, or animation. Router pointer
capture/rollback preserves the previous screen on construction failure and
clears every provisioning pointer when another screen becomes active.

### Text and color mapping

| State | Instruction | Status | Color |
|---|---|---|---|
| `STARTING` | Prepare your phone | Starting setup... | Blue |
| `WAITING_FOR_PHONE` | Scan to connect | Waiting for phone | Blue |
| `CREDENTIAL_RECEIVED` | Wi-Fi received | Checking... | Blue |
| `CONNECTING_WIFI` | Connecting to Wi-Fi | Connecting... | Blue |
| `WAITING_FOR_IP` | Connected to router | Getting IP... | Blue |
| `SAVING_CONFIG` | Saving settings | Saving... | Yellow |
| `CLEANING_UP` | Setup complete | Finishing... | Yellow |
| `SUCCESS` | Wi-Fi configured | Connected | Green |
| `FAILED` | Check Wi-Fi details | Connection failed | Red |
| `TIMEOUT` | Setup expired | Timed out | Red |
| `RETRYING` | Starting a new session | Retrying... | Yellow |

`last_error` and `wifi_disconnect_reason` are available only for debug logs
and future policy. Numeric internal values are not shown to normal users.

## Application Routing

The network coordinator owns config-driven and normal-network routing policy.
It posts screen requests but never renders or calls LVGL.

| Trigger | Route or behavior |
|---|---|
| Config `VALID` after verification/migration | `BOOT` -> `WIFI_STATUS` after verified normal `GOT_IP` -> `SENSOR_DASHBOARD` after the existing timeout |
| Config `NOT_CONFIGURED` | `PROVISIONING` screen directly |
| Config migration | Resolve migration, then use `BOOT` for the final valid path |
| Config inspection or unrecoverable integrity error | Best-effort `BOOT` placeholder plus safe error logging |
| Provisioning status update | Cache/render only; no implicit screen change |
| Verified provisioning success | `SUCCESS` dwell for 1500 ms -> explicit `WIFI_STATUS` request |
| Wi-Fi status update | Cache/render only; no implicit screen change |
| Sensor status update | Cache/render only; no implicit screen change |
| Cloud status update | Cache/render only; no implicit screen change |
| Wi-Fi screen timeout | Explicit deferred `SENSOR_DASHBOARD` request |

While the coordinator remains in `PROVISIONING`, association, DHCP, and
disconnect events update only the provisioning model. They cannot promote the
application to `ONLINE` or request a normal screen. After verified persistence,
BLE cleanup, and connection adoption, the coordinator posts `SUCCESS`, waits
1500 ms, and requests `WIFI_STATUS`; its existing 10-second timer later requests
`SENSOR_DASHBOARD`.

## Error Handling

- Invalid or `NONE` screen requests return `ESP_ERR_INVALID_ARG`.
- NULL or invalid provisioning status returns `ESP_ERR_INVALID_ARG`.
- NULL, empty, or unterminated QR payload returns `ESP_ERR_INVALID_ARG`.
- Requests before `app_gui_init()` return `ESP_ERR_INVALID_STATE`.
- A full command queue returns `ESP_ERR_TIMEOUT`.
- Provisioning status and QR queues use length-one overwrite; an unexpected
  overwrite failure returns `ESP_FAIL`.
- Missing LVGL roots return `ESP_ERR_INVALID_STATE`.
- Widget or timer allocation failures return `ESP_ERR_NO_MEM`.
- QR encoding failure hides the QR object, emits only a generic error, and
  never prints the payload.
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

## Phase 6.4.2 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

Phase 6.4.2 intentionally does not implement:

- QR code, `lv_qrcode`, payload generation, service-name display, or QR
  configuration;
- real provisioning-manager or coordinator status integration;
- automatic `SUCCESS -> WIFI_STATUS` or `FAILED -> PROVISIONING` routing;
- provisioning timeout recovery or retry policy;
- BLE, Wi-Fi manager, config manager, cloud, Firebase, factory-reset, touch,
  animation, progress-bar, or dashboard changes.

## Phase 6.4.3 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

Implemented:

- Espressif-compatible Security 1 QR JSON from the active BLE service;
- dedicated copied-payload queue and cache;
- official LVGL 9 `lv_qrcode` widget;
- maximum 2-pixel integer module scale with a measured quiet zone;
- coordinator publication only after provisioning start succeeds;
- no QR payload, PoP, password, or credential logging.

Phase 6.4.3 intentionally does not implement provisioning progress producers,
automatic success/failure screen transitions, timeout recovery UI, factory
reset, touch, animation, progress bars, or dashboard redesign. Those remain
outside this checkpoint.

## Phase 6.4.4 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

Implemented:

- dedicated length-one overwrite queue for real provisioning status;
- session-owned QR cache with explicit secure invalidation;
- manager, Wi-Fi callback, and coordinator progress mapping;
- non-terminal `FAILED` presentation for an ordinary credential failure;
- bounded `TIMEOUT` cleanup;
- verified `SAVING_CONFIG -> CLEANING_UP -> SUCCESS` flow;
- `SUCCESS` dwell followed by `WIFI_STATUS`, then the existing dashboard timer.

Automatic retry/restart and the `RETRYING` producer were intentionally
deferred to Phase 6.4.5 at this checkpoint. Factory reset, touch, animation,
progress bars, and dashboard redesign remain outside Phase 6.4.4.

## Phase 6.4.5 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

Implemented:

- non-zero session generation in provisioning status and QR models;
- generation-aware explicit QR clear;
- strict-newer generation cache invalidation;
- stale status/QR/clear rejection before rendering;
- `TIMEOUT -> CLEANING_UP -> RETRYING -> STARTING` presentation without
  recreating the screen;
- final exhaustion that leaves the Provisioning Screen active, hides the QR,
  and preserves the final terminal state.

Hardware must still verify message ordering under real BLE cleanup, QR
replacement after retry, stale-event injection, and long-run LVGL/heap
stability. Phase 6.4 remains incomplete.

## Manual Acceptance Tests

Run any temporary state driver outside the UI task, call only public
`app_gui` APIs, and remove it before committing production behavior.

1. Activate `PROVISIONING`; verify `Wi-Fi Setup`, `Prepare your phone`, and
   `Starting setup...` with no clipping or white flash.
2. Exercise all provisioning states including `RETRYING`; verify text/color
   changes without
   object recreation, crash, watchdog, or LVGL assertion.
3. Leave and re-enter `PROVISIONING` while BLE remains active; verify the same
   QR is still rendered. Then explicitly clear it and verify the QR hides.
4. Request `PROVISIONING` repeatedly; verify no duplicate objects, reset, leak,
   or crash.
5. Cycle `PROVISIONING -> WIFI_STATUS -> PROVISIONING ->
   SENSOR_DASHBOARD -> PROVISIONING`; verify no stale pointers, blank screen,
   white flash, or lost cached status.
6. Cycle states for at least 15 minutes; verify no heap-growth trend, stack
   warning, watchdog, LVGL assertion, or crash.
7. On a reset device, scan the displayed QR with Espressif Provisioning,
   verify the app resolves the advertised `PROV_XXXXXX` service without manual
   PoP entry, and provision Wi-Fi successfully.
8. Repeat with a long SSID/password and from normal viewing distance; verify
   module edges and the full white quiet zone remain visible.
9. Verify the serial log contains no QR JSON, PoP, password, session key
   material, or credentials.
10. Submit a wrong password, verify `FAILED` is shown while the QR/session
    remains usable, then submit correct credentials without rebooting.
11. Verify successful provisioning shows `SAVING_CONFIG`, `CLEANING_UP`, and
    `SUCCESS`, holds success for about 1500 ms, then shows `WIFI_STATUS` and
    later `SENSOR_DASHBOARD`.
12. Allow a session to expire; verify `TIMEOUT`, failure dwell,
    `CLEANING_UP`, `RETRYING`, a new QR, and `WAITING_FOR_PHONE` without a
    screen rebuild or stale old QR.
13. Delay an old generation's `FAILED`, `STOPPED`, and QR-clear messages until
    the next generation is active; verify they cannot hide or overwrite the
    new QR and `WAITING_FOR_PHONE`.
14. Exhaust all configured sessions; verify the final QR stays hidden and the
    final `TIMEOUT` or `FAILED` remains visible on the Provisioning Screen.

Phase 6.4 is not complete.
