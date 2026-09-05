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
Phase 7.4 adds a modal factory-reset result screen plus an exact,
transaction-aware presentation acknowledgment for controlled restart timing.
Task 3 adds the synchronized local clock and date to the sensor dashboard;
the existing UI task refreshes those labels through the LVGL timer handler.
Phase 12.5 adds a temporary Xiaozhi interaction-validation screen. It presents
only copied WebSocket validation facts; it does not add a production voice
assistant, microphone, speaker, or independent GUI task.

## Application Screens

| Screen ID | Current role |
|---|---|
| `APP_GUI_SCREEN_NONE` | Internal state before the first successful route; never a public request target. |
| `APP_GUI_SCREEN_BOOT` | Static `Smart Gateway` / `Starting...` placeholder. |
| `APP_GUI_SCREEN_PROVISIONING` | Stable provisioning layout with a scannable QR code, instruction/status labels, and state indicator. |
| `APP_GUI_SCREEN_WIFI_STATUS` | Existing Wi-Fi mode, SSID, and IPv4 screen. |
| `APP_GUI_SCREEN_SENSOR_DASHBOARD` | Sensor dashboard with synchronized local time/date in its left header, temperature/humidity below, and Wi-Fi, cloud, sensor, and audio summaries in the right status column. |
| `APP_GUI_SCREEN_XIAOZHI` | Project-owned Xiaozhi voice presentation: connection state, actual-microphone recording duration, and bounded USER/ASSISTANT transcript. It is entered through the existing explicit screen-request API. |
| `APP_GUI_SCREEN_RESET_RESULT` | Factory-reset success or failure result; entered only through `app_gui_show_reset_result()`. |

The old `APP_GUI_SCREEN_WIFI` and `APP_GUI_SCREEN_SENSOR` identifiers were
renamed directly. No compatibility aliases are retained.

## Queues And Public API

`app_gui_init()` creates all queues as one initialization transaction:

- Command queue: length 8, carrying copied screen requests and copied
  reset-result commands.
- Provisioning status queue: length 1, newest complete snapshot overwrites the
  pending value.
- Provisioning QR queue: length 1, newest payload-or-unavailable message
  overwrites the pending value.
- Wi-Fi status queue: length 1, newest snapshot overwrites the pending value.
- Sensor status queue: length 5, producers never wait.
- Audio status queue: length 1, newest state snapshot overwrites the pending value.
- Cloud status queue: length 1, newest snapshot overwrites the pending value.
- Xiaozhi status queue: length 1, newest copied temporary-validation snapshot
  overwrites the pending value.

If any allocation fails, every queue created by that attempt is deleted, all
handles return to `NULL`, and `app_gui_init()` returns `ESP_ERR_NO_MEM`.

### Sensor dashboard audio status

The audio status queue is consumed only by the GUI task. The dashboard's right
status column maps copied manager state as follows; a non-OK `last_error` while
the manager is otherwise `IDLE` is rendered as `Audio: ERR`.

| Audio UI state | Dashboard text | Color |
|---|---|---|
| `UNAVAILABLE` | `Audio: --` | Gray |
| `READY` | `Audio: Ready` | Blue |
| `IDLE` | `Audio: Idle` | Gray |
| `RECORDING` | `Audio: REC` | Red |
| `PROCESSING` | `Audio: DSP` | Yellow |
| `PLAYBACK` | `Audio: PLAY` | Green |
| `ERROR` | `Audio: ERR` | Red |

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
| `app_gui_post_audio_status()` | Replace the pending non-sensitive audio state without calling LVGL. |
| `app_gui_post_cloud_status()` | Replace the pending cloud model without calling LVGL. |
| `app_gui_post_xiaozhi_status()` | Replace the pending bounded Xiaozhi validation model without calling LVGL or routing a screen. |
| `app_gui_show_reset_result()` | Validate, copy, and enqueue one reset result without calling LVGL. |
| `app_gui_is_reset_result_presented()` | Non-blockingly inspect whether the exact transaction completed a GUI presentation cycle. |

`app_gui_request_screen()` is safe from normal task and task-context callback
code. It does not wait for construction and is not ISR-safe. `ESP_OK` means
only that the command was queued. It returns `ESP_ERR_TIMEOUT` when the command
queue is full. The generic request API rejects `APP_GUI_SCREEN_RESET_RESULT`;
only a validated reset-result command may enter that screen.

`app_gui_show_reset_result()` follows the same copied, zero-wait producer
contract. `SUCCESS` is valid only with `ESP_OK`; `FAILED` requires a non-OK
error. `app_gui_is_reset_result_presented()` reads a transaction ID under the
short GUI state critical section and never waits, takes the LVGL mutex, or
calls LVGL.

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
-> consume/cache/render latest Xiaozhi validation status when its screen is active
-> take LVGL mutex
-> lv_timer_handler()
-> release LVGL mutex
-> acknowledge the latest matching reset transaction rendered in this pass
```

`app_gui_activate_screen()` is the only normal-runtime writer of the
authoritative active-screen ID. For each non-duplicate request it:

1. Keeps the current root loaded.
2. Creates and builds a separate target root.
3. Renders the latest cached model into the completed target root.
4. Preserves the current root and restores its widget references if target
   construction, cached rendering, or Wi-Fi timer allocation fails.
5. Loads the target only after construction and rendering succeed.
6. Updates the active-screen ID only after the target is valid.
7. Pauses timers that belong to inactive screens, including the Xiaozhi
   duration timer before its root can be deleted.
8. Keeps the new screen's widget references and deletes the previous inactive
   root.

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
callbacks never call LVGL. The temporary Xiaozhi observer follows the same
rule: its worker/event-loop callback only posts a copied status snapshot.

The Wi-Fi timeout callback already runs inside `lv_timer_handler()` while the
mutex is held. It therefore only posts a non-blocking
`APP_GUI_SCREEN_SENSOR_DASHBOARD` command and pauses the timer. The command is
processed during the next UI iteration; the callback never tries to take the
mutex recursively.

## Task 3 Clock And Date

`app_gui` reads `time_manager_get_local_time()` directly from a private LVGL
timer that runs every 1000 ms only while `SENSOR_DASHBOARD` is active. The
timer is serviced by the existing UI task inside `lv_timer_handler()`; there is
no clock FreeRTOS task, callback-to-GUI path, or screen-command producer.

Before the first successful SNTP synchronization, the header shows
`--:--:--` and `--/--/----`. After synchronization, each refresh formats the
ESP-IDF local system clock as `HH:MM:SS` and `DD/MM/YYYY`, so the last known
clock continues advancing while Wi-Fi is offline. `app_gui` never adds UTC+7
or increments a local counter.

The timer stores no LVGL widget pointer in its user data. Screen activation
pauses it before an old dashboard root is deleted, then renders and resumes it
only after a new dashboard root is loaded. This keeps clock refreshes within
the UI ownership and screen-lifecycle rules. The dependency direction is
read-only: `app_gui -> time_manager`; `time_manager` remains independent of
GUI/LVGL.

## Xiaozhi Lifecycle UI

The interaction screen is separate from provisioning, Wi-Fi status, and the
sensor dashboard. It uses the existing `app_gui_request_screen()` API; there
is no new touch/menu/button path. Production lifecycle updates always copy the
latest snapshot, but only a real `READY -> LISTENING` microphone-capture
transition may select `XIAOZHI`. When that interaction reaches terminal
`READY`, `CONNECTING`, or `IDLE`, the adapter returns to the sensor dashboard
after three seconds. A temporarily full GUI command queue is retried without
abandoning this return. Startup and reconnect states never claim the
interaction screen. Leaving the screen does not stop, cancel, or restart the
Xiaozhi session; returning to it renders the latest cached snapshot.

```text
voice_assistant copied session/audio snapshot
    -> voice_assistant_ui_model derives presentation state and capture timestamps
    -> voice_assistant_ui_gui_adapter copies project GUI model
    -> app_gui_post_xiaozhi_status() length-one overwrite queue
    -> app_gui UI task caches status
    -> render only while APP_GUI_SCREEN_XIAOZHI is active
```

`ui_xiaozhi_status_t` has the states `DISCONNECTED`, `CONNECTING`, `READY`,
`LISTENING`, `PROCESSING`, `RESPONDING`, `RECOVERING`, and `ERROR`, start/stop timestamps from
`esp_timer_get_time()`, a non-sensitive `esp_err_t`, and separate 192-byte
NUL-terminated USER/ASSISTANT buffers. `LISTENING` is rendered as
`RECORDING` only after real microphone capture is active. The GUI timer
computes the live duration from that start timestamp and freezes it after
capture stops; background code never sends 100 ms duration messages.

The 160x128 layout has a connection/state indicator, state/detail line,
`RECORD mm:ss.t` while capture is active (then the frozen `LISTEN` duration),
and fixed USER/XZ transcript regions. The model keeps at most 191 bytes per
role plus NUL. The on-screen regions use fixed-size clipping so long text
cannot grow, relocate, or retain a stale LVGL object; truncation flags remain
available for diagnostics without printing text in logs.

One LVGL timer runs every 100 ms only while `XIAOZHI` is active. It stores no
widget pointer in user data, is paused before another root is deleted, and is
resumed only after a completed Xiaozhi root is loaded. No new FreeRTOS task,
queue message per timer tick, or recursive LVGL mutex acquisition is used.
P2-D receive plumbing and P2-E/P2-F WebSocket/audio validation behavior remain
unchanged.

## Model Caching

The GUI retains the latest complete:

- `ui_provisioning_status_t` plus an availability flag;
- `ui_provisioning_qr_payload_t` plus an availability flag;
- `ui_wifi_status_t` plus an availability flag;
- `ui_sensor_status_t` plus an availability flag;
- `ui_cloud_status_t` plus an availability flag.
- `ui_xiaozhi_status_t` plus an availability flag;
- `ui_reset_status_t` plus an availability flag.

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
- Xiaozhi validation updates render only on `XIAOZHI`.
- Reset results render only through the reset-result command and screen.
- Updates received for inactive screens only refresh cached models and never
  touch deleted LVGL pointers.

Entering `PROVISIONING` renders the latest provisioning model, or the default
`STARTING` model before the first update, and renders the latest valid QR
payload when available. Entering `WIFI_STATUS` renders the latest Wi-Fi model.
Entering `SENSOR_DASHBOARD` renders the latest sensor, Wi-Fi summary, cloud
models, and the current time/date header.
Entering `XIAOZHI` renders its newest complete cached snapshot, or a safe
`DISCONNECTED` default before the first validation update.

## Phase 7.4 Reset Result UI

The reset coordinator supplies a non-zero boot-local transaction ID with every
terminal reset result. The command queue copies the complete result. Only the
UI task validates its cached copy, creates or updates the reset screen, and
touches LVGL objects.

```text
validated reset result
    -> render status into a complete target root
    -> load RESET_RESULT and lock ordinary routing
    -> run the following lv_timer_handler() pass
    -> publish acknowledgment for the exact transaction ID
```

Success shows `Wi-Fi cleared` and `Restarting into setup...` in green. Failure
shows `Reset failed`, the non-sensitive `esp_err_t` name, and a release/retry
instruction in red. Both results are modal: ordinary screen commands are
ignored, while a newer reset-result command remains allowed so a failed attempt
can be replaced after release and retry.

`presented` means that the matching content was applied to valid LVGL objects
and the next `lv_timer_handler()` call returned. It is not a physical LCD
transfer acknowledgment. The reset coordinator waits at most 500 ms for this
signal, holds confirmed success for 1500 ms, and otherwise uses its finite
500 ms reboot fallback. Queue, construction, or acknowledgment failure cannot
suppress reboot after persistent cleanup has been verified. A storage failure
never reboots.

**Phase 7.4 is implemented and build-verified; hardware acceptance is pending.**

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
active. A credential failure is displayed for the configured dwell, then the
coordinator stops that failed framework generation; its terminal clear removes
the old QR before the replacement generation publishes a new payload.

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
| Config `VALID` after verification/migration | `BOOT` -> `WIFI_STATUS` immediately after verified normal `GOT_IP`, or after 60 seconds if stored Wi-Fi is still unavailable -> `SENSOR_DASHBOARD` after the existing timeout |
| Config `NOT_CONFIGURED` | `PROVISIONING` screen directly |
| Config migration | Resolve migration, then use `BOOT` for the final valid path |
| Config inspection or unrecoverable integrity error | Best-effort `BOOT` placeholder plus safe error logging |
| Provisioning status update | Cache/render only; no implicit screen change |
| Verified provisioning success | `SUCCESS` dwell for 1500 ms -> explicit `WIFI_STATUS` request |
| Wi-Fi status update | Cache/render only; no implicit screen change |
| Sensor status update | Cache/render only; no implicit screen change |
| Audio status update | Cache/render only; no implicit screen change |
| Cloud status update | Cache/render only; no implicit screen change |
| Xiaozhi validation status update | Cache/render only on `XIAOZHI`; no implicit screen change and no lifecycle action |
| Wi-Fi screen timeout | Explicit deferred `SENSOR_DASHBOARD` request |
| Reset result | Dedicated copied command -> modal `RESET_RESULT`; ordinary routing remains blocked, while a newer reset result may replace the current one before verified success reboots |

While the coordinator remains in `PROVISIONING`, association, DHCP, and
disconnect events update only the provisioning model. They cannot promote the
application to `ONLINE` or request a normal screen. After verified persistence,
BLE cleanup, and connection adoption, the coordinator posts `SUCCESS`, waits
1500 ms, and requests `WIFI_STATUS`; its existing 10-second timer later requests
`SENSOR_DASHBOARD`.

## Error Handling

- Invalid or `NONE` screen requests return `ESP_ERR_INVALID_ARG`.
- NULL or invalid provisioning status returns `ESP_ERR_INVALID_ARG`.
- Invalid Xiaozhi state, timestamp ordering, error/state pairing, or
  unterminated transcript returns `ESP_ERR_INVALID_ARG`.
- NULL, empty, or unterminated QR payload returns `ESP_ERR_INVALID_ARG`.
- A zero reset transaction ID or inconsistent reset state/error pair returns
  `ESP_ERR_INVALID_ARG`.
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
- Screen failures do not themselves trigger or suppress reboot and do not
  change network policy.
- Reset-screen failure leaves the previous root and widget references valid;
  the reset coordinator owns the finite reboot fallback.

## Initialization Order

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(app_gui_init());
ESP_ERROR_CHECK(app_gui_start_ui_task());
#if CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE
/* Register the temporary Xiaozhi observer; failure is logged but non-fatal. */
xiaozhi_foundation_register_ui_status_callback(...);
#endif
ESP_ERROR_CHECK(app_network_coordinator_init(&network_config));
ESP_ERROR_CHECK(app_network_coordinator_start());
```

`main` does not create an initial screen directly. The coordinator requests the
initial screen only after resolving the final configuration state. With the
temporary Phase 12 validation gate at its default `n`, the observer is not
registered and an `ONLINE` transition does not request `APP_GUI_SCREEN_XIAOZHI`.

## Phase 6.4.1 Historical Checkpoint

**IMPLEMENTED / HARDWARE TEST PENDING**

At the 6.4.1 checkpoint, the following later-phase behavior was intentionally
outside scope:

- QR code or `lv_qrcode`;
- provisioning payload, service name, PoP, or security changes;
- provisioning progress/retry/timeout UI;
- successful provisioning-to-`WIFI_STATUS` routing;
- factory reset, touch navigation, animation, progress bar, or dashboard
  redesign;
- BLE lifecycle, Wi-Fi retry, Firebase, or cloud-task changes.

QR rendering, real progress, success routing, and retry states were added by
6.4.3 through 6.4.5 without changing the centralized router.

## Phase 6.4.2 Historical Checkpoint

**IMPLEMENTED / HARDWARE TEST PENDING**

At the 6.4.2 checkpoint, the following later-phase behavior was intentionally
outside scope:

- QR code, `lv_qrcode`, payload generation, service-name display, or QR
  configuration;
- real provisioning-manager or coordinator status integration;
- automatic `SUCCESS -> WIFI_STATUS` or `FAILED -> PROVISIONING` routing;
- provisioning timeout recovery or retry policy;
- BLE, Wi-Fi manager, config manager, cloud, Firebase, factory-reset, touch,
  animation, progress-bar, or dashboard changes.

Those provisioning producers and policies are now integrated; factory reset,
touch, animation, and dashboard redesign remain out of scope.

## Phase 6.4.3 Historical Checkpoint

**IMPLEMENTED / HARDWARE TEST PENDING**

Implemented:

- Espressif-compatible Security 1 QR JSON from the active BLE service;
- dedicated copied-payload queue and cache;
- official LVGL 9 `lv_qrcode` widget;
- maximum 2-pixel integer module scale with a measured quiet zone;
- coordinator publication only after provisioning start succeeds;
- no QR payload, PoP, password, or credential logging.

Real provisioning producers, success/failure routing, and timeout recovery
were added in 6.4.4 and 6.4.5. Factory reset, touch, animation, progress bars,
and dashboard redesign remain outside Phase 6.4.

## Phase 6.4.4 Historical Checkpoint

**IMPLEMENTED / HARDWARE TEST PENDING**

Implemented:

- dedicated length-one overwrite queue for real provisioning status;
- session-owned QR cache with explicit secure invalidation;
- manager, Wi-Fi callback, and coordinator progress mapping;
- non-terminal `FAILED` presentation for an ordinary credential failure;
- bounded `TIMEOUT` cleanup;
- verified `SAVING_CONFIG -> CLEANING_UP -> SUCCESS` flow;
- `SUCCESS` dwell followed by `WIFI_STATUS`, then the existing dashboard timer.

Automatic retry/restart and the `RETRYING` producer were added in Phase 6.4.5.
Factory reset, touch, animation, progress bars, and dashboard redesign remain
outside Phase 6.4.

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
stability. Final Phase 6.4 hardware acceptance remains pending.

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
10. Submit a wrong password, verify `FAILED -> CLEANING_UP -> RETRYING`, then
    verify the old QR is cleared and submit correct credentials through the new
    QR/generation without rebooting.
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
15. From normal configured runtime, hold the reset button; verify the green
    reset-success screen appears before reboot and remains readable for about
    1500 ms after its presentation acknowledgment.
16. Inject driver cleanup, application clear, and verification failures one at
    a time; verify the red failure result, no reboot, and re-arm only after
    button release.
17. Fill the GUI command queue or inject reset-screen construction failure;
    after verified persistent cleanup, verify the bounded fallback still
    reboots without a watchdog or reset loop.
18. After a displayed failure, release and long-press again; verify the newer
    transaction replaces the old result and only its exact acknowledgment can
    select the 1500 ms dwell.
19. Repeat successful and failed cycles while monitoring UI-task stack and
    heap minima; verify no stale pointer, LVGL assertion, or growth trend.
20. Before the first successful SNTP synchronization, enter
    `SENSOR_DASHBOARD`; verify the header shows `--:--:--` and `--/--/----`,
    never an unsynchronized 1970-era time.
21. After SNTP synchronization, verify the header uses Vietnam local time in
    `HH:MM:SS` and `DD/MM/YYYY` format and advances about once per second
    without clipping or overlap.
22. Disconnect Wi-Fi after a successful synchronization, then cycle into and
    out of `SENSOR_DASHBOARD`; verify the clock continues advancing and no
    LVGL assertion, stale pointer, or duplicate timer behavior occurs.
23. Request `XIAOZHI`, then inject each copied state through the public API;
    verify connection/state/color, safe disconnected/error text, and no
    automatic route away from the current screen.
24. During Phase-15 hardware validation, verify actual-microphone `RECORDING`
    advances near 100 ms resolution, freezes after `PROCESSING`/`RESPONDING`/
    `ERROR`, and shows
    bounded USER and XZ transcript snippets without new object creation.
25. Navigate `XIAOZHI -> SENSOR_DASHBOARD -> XIAOZHI` during a validation
    attempt. Verify the validation continues independently, latest status is
    restored on return, and no stale-pointer/LVGL assertion/heap-growth trend
    appears under repeated transitions.

## Phase 6.4.7 Closure Status

**IMPLEMENTED / HARDWARE REGRESSION PENDING**

The UI task remains the sole owner of LVGL calls, screen roots, widgets,
timers, generation-aware provisioning caches, and latest-value GUI queues.
Generic screen changes do not invalidate an active session; only an explicit
matching/newer generation clear can retire its QR. Construction failure keeps
the previous screen and object references valid. The complete A-N hardware
matrix is maintained in the project roadmap.

**Phase 6.4 — IMPLEMENTED / FINAL HARDWARE ACCEPTANCE PENDING**
