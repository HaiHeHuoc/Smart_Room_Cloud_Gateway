# Application Network Coordinator

## Purpose

`app_network_coordinator` owns application-level network boot policy. It
coordinates persistent Wi-Fi configuration, BLE provisioning, and the handoff
to `wifi_manager` without taking ownership of storage, transport internals,
reconnect policy, or GUI rendering.

## Ownership

The coordinator:

- Resolves the current `config_manager` Wi-Fi integrity state.
- Migrates a supported legacy configuration before use.
- Starts a stored Station connection when configuration is valid.
- Starts bounded BLE provisioning only for `NOT_CONFIGURED`.
- Runs at most three provisioning sessions by default, including the initial
  session, without recursion, reboot, or one task per retry.
- Copies the exact active BLE QR payload into the GUI's dedicated latest-value
  queue immediately after provisioning starts.
- Registers the manager's single progress callback and maps copied lifecycle
  facts into provisioning UI states.
- Maps the existing Wi-Fi callback's association, DHCP, and disconnect
  snapshots into provisioning progress without taking Wi-Fi ownership.
- Maps the framework's `WIFI_CONNECTED` progress to `WAITING_FOR_IP`; only the
  coordinator task can advance a verified queue item to `SAVING_CONFIG`.
- Reconciles DHCP replies that arrive immediately after clean BLE teardown
  through one generation-bound, five-second, reset-aware settle window.
- Requests the initial application screen from the final verified
  configuration state.
- Persists provisioned credentials through `config_manager`.
- Verifies NVS state and credential read-back.
- Waits for provisioning cleanup and asks `wifi_manager` to adopt the active
  connection.
- Tracks application-level Wi-Fi readiness after boot, including later
  connecting, online, and offline transitions.

The coordinator does not:

- Access NVS directly.
- Own Wi-Fi event handlers or reconnect behavior.
- Own BLE transport internals.
- Call LVGL, build screens, or render GUI objects. It may only post
  asynchronous `app_gui` screen requests and copied status/QR models.
- Log passwords, PoP values, tokens, or credential contents.

## Public API

| API | Responsibility |
|---|---|
| `app_network_coordinator_init()` | Copy timing configuration and enter `READY`. |
| `app_network_coordinator_start()` | Schedule the one-shot coordinator task and return immediately. |
| `app_network_coordinator_prepare_for_factory_reset()` | Close the reset gate, quiesce provisioning, disable reconnect, and wait within one caller-supplied deadline for Station detach. |
| `app_network_coordinator_get_state()` | Copy the thread-safe lifecycle state. |
| `app_network_coordinator_state_to_string()` | Convert a state to readable text. |
| `app_network_coordinator_notify_wifi_event()` | Apply a short Wi-Fi notification to provisioning progress or normal runtime state, including the raw disconnect reason. |

## State Flow

```text
UNINITIALIZED
    -> READY
    -> STARTING
    -> RESOLVING_CONFIG
       -> CONNECTING
          <-> ONLINE
          <-> OFFLINE
       -> PROVISIONING -> ONLINE <-> OFFLINE
       -> FAILED
```

`CONNECTING` represents an asynchronous stored-credential connection request.
`ONLINE` requires a valid IPv4 address. After normal Station ownership begins,
the composition root forwards Wi-Fi manager snapshots so later connection,
DHCP, disconnection, and retry events keep the coordinator state current.
During provisioning, Wi-Fi events update only the GUI progress model. They
cannot promote coordinator state to `ONLINE`, request `WIFI_STATUS`, start
cloud work, or enable reconnect ownership. The application becomes `ONLINE`
only after credentials are persisted, BLE is cleaned up, and the active
connection is adopted.

For an unconfigured device, startup ordering is:

```text
request PROVISIONING screen
    -> STARTING
    -> initialize/start BLE service
    -> copy active QR payload from provisioning_manager
    -> post copied payload to app_gui
    -> WAITING_FOR_PHONE
    -> CREDENTIAL_RECEIVED
    -> CONNECTING_WIFI
    -> WAITING_FOR_IP after Station association
    -> framework GOT_IP queues verified credentials
    -> SAVING_CONFIG
    -> CLEANING_UP
    -> adopt the valid Station/IPv4 connection
    -> coordinator ONLINE
    -> clear the session QR cache
    -> SUCCESS
    -> 1500 ms dwell
    -> request WIFI_STATUS
    -> best-effort terminal BLE memory release
```

A retryable timeout or clean terminal session failure follows:

```text
TIMEOUT or FAILED
    -> 1000 ms failure dwell
    -> CLEANING_UP
    -> manager STOPPED
    -> generation-aware QR invalidation
    -> RETRYING
    -> 1500 ms backoff
    -> STARTING with a new generation
    -> new QR
    -> WAITING_FOR_PHONE
```

When association succeeds but DHCP responds only after BLE stops, the grace
boundary follows this recovery branch:

```text
30-second IPv4 grace expires with handoff pending
    -> final non-blocking verified-queue drains
    -> claim reset exclusion and arm the exact generation
    -> stop/deinitialize provisioning to STOPPED
    -> final framework-success queue drain
    -> poll Station for at most 5 seconds
    -> require CONNECTED + IPv4 + exact pending SSID
    -> promote to the ordinary verified queue
    -> reuse SAVING_CONFIG -> read-back -> adoption path
```

The coordinator remains `PROVISIONING` throughout intermediate failures and
backoff. It changes to `FAILED` only after retry exhaustion, a nonretryable
failure, or cleanup that cannot reach `STOPPED`.

## Active Factory-Reset Coordination

**PHASE 7.5 IMPLEMENTED / BUILD VERIFIED / HARDWARE ACCEPTANCE PENDING**

`app_network_coordinator_prepare_for_factory_reset(timeout_ms)` is the
synchronous task-context handoff to `app_reset_coordinator`. It sets an
application reset gate before inspecting the provisioning manager. The gate
prevents new boot resolution, stored connection startup, provisioning
sessions, retries, credential persistence, adoption, normal success routing,
and runtime Wi-Fi/dashboard routing.

Short reset-exclusion claims protect operations whose external side effects
must finish before erasure. Late-DHCP teardown and settle retain the claim for
their bounded transaction, poll the reset gate, and abort without persistence
if reset wins. The coordinator never holds its critical lock across manager,
NVS, Wi-Fi, GUI, logging, or delay APIs. A verified queue item is received and
securely cleared when the reset gate wins; a retained unverified late copy is
discarded by generation before Station detach and persistent erasure.

| Provisioning state | Preparation action |
|---|---|
| `UNINITIALIZED`, `READY`, `STOPPED` | Require no active exclusion; drain a verified handoff or discard an armed late copy, then disconnect Station and wait for detach. |
| `STARTING`, `STOPPING` | Poll with the configured finite period until safe or timed out. |
| `ACTIVE` | Request `provisioning_manager_stop()` at the atomic ACTIVE boundary, then wait for cleanup. |
| `FAILED` | Fail safe; do not authorize persistent erasure. |

Success requires both provisioning and Station lifecycle quiescence and leaves
the gate asserted until reboot. Failure rolls back only a gate created by that
call and does not erase configuration or call persistent Wi-Fi reset or GUI
APIs. No new task, queue, or dynamic allocation is introduced.

QR publication is best-effort and never promotes coordinator state, starts a
Wi-Fi connection, or changes the active screen.

`app_network_coordinator_start()` does not wait for provisioning. The dedicated
task performs the bounded receive, persistence, cleanup, and adoption flow so
`app_main()` can continue initializing independent services.

The composition root initializes cloud state/queues and starts local sensor
sampling before scheduling this task. It then defers the memory-heavy cloud
task until the coordinator reaches `CONNECTING` for a stored connection or
`ONLINE` after provisioning cleanup and adoption. `PROVISIONING` never opens
the cloud gate, so BLE resources do not overlap with cloud TLS activity.

## Boot Policy

| Config state | Action |
|---|---|
| `VALID` | Load a temporary copy and call `wifi_manager_connect()`. |
| `NOT_CONFIGURED` | Run BLE provisioning with a finite timeout. |
| `MIGRATION_REQUIRED` | Migrate once, re-inspect, and require `VALID`. |
| `INCOMPLETE` | Preserve data and fail. |
| `INVALID_DATA` | Preserve data and fail. |
| `UNSUPPORTED_VERSION` | Preserve data and fail. |
| `UNKNOWN` | Fail without treating the device as unconfigured. |

Screen routing follows the final result:

- `NOT_CONFIGURED` queues the `PROVISIONING` placeholder before BLE
  provisioning begins.
- `VALID`, including a successfully migrated configuration, queues `BOOT`.
  If stored Wi-Fi has not reached `ONLINE` after 60 seconds and the GUI is
  still on `BOOT`, the coordinator queues `WIFI_STATUS`. That screen renders
  the latest cached disconnected/retry state while `wifi_manager` continues
  its existing reconnect policy.
- Inspection failures and non-provisionable integrity states make a
  best-effort `BOOT` request before returning the existing policy error.
- A normal stored-credential `CONNECTING -> ONLINE` transition queues
  `WIFI_STATUS` immediately only while `BOOT` is active, without waiting for
  the 60-second fallback. Runtime reconnects preserve any explicit
  provisioning, reset, dashboard, or voice route.
- Provisioning Wi-Fi events update progress only; the verified success path
  requests `WIFI_STATUS` after cleanup, adoption, and a 1500 ms success dwell.

## Threading And Security

- Lifecycle state and copied configuration are protected by a short critical
  section.
- The one-shot coordinator task uses a 6 KB stack at priority 4 and deletes
  itself after boot orchestration succeeds or fails.
- Manager, NVS, Wi-Fi, logging, and callback APIs are never called while that
  critical section is held.
- `app_network_coordinator_notify_wifi_event()` performs no allocation,
  blocking wait, Wi-Fi call, or LVGL call. A verified normal transition to
  `ONLINE` may post one non-blocking `WIFI_STATUS` screen request only when
  `BOOT` remains active, so the API remains suitable for the normal
  task-context Wi-Fi status callback.
- The manager progress callback posts only copied latest-value GUI messages.
  It performs no wait, persistence, Wi-Fi operation, or LVGL call.
- `PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTED` publishes
  `WAITING_FOR_IP`, matching the `e66adb3` progress mapping. The verified queue
  still gates persistence and adoption.
- Provisioning waits use configured finite session, grace, and poll periods.
- A pending credential handoff uses the remaining session time first. If it is
  still pending when the session deadline expires, it receives one finite IPv4
  grace window. Grace expiry performs final queue drains, clean BLE teardown,
  and at most five additional reset-aware seconds for DHCP completion.
- Late credentials stay RAM-only and generation-bound. Persistence requires
  manager `STOPPED`, Station `CONNECTED`, valid IPv4, exact SSID match, and
  successful delivery through the ordinary verified queue.
- Timeout, mismatch, internal error, or reset drains verified queue storage and
  zeroizes any armed pending copy. Non-reset failure detaches the unmanaged
  Station before retry.
- Temporary SSID/password buffers are securely overwritten on all completed
  paths.
- The temporary QR copy is overwritten immediately after its non-blocking GUI
  post and its contents are never logged.
- GUI status updates remain event-driven through the registered
  `wifi_manager` callback and the manager progress callback. Config-driven
  initial routing and verified screen transitions use the independent
  `app_gui` command queue.

## Configuration

```c
static const app_network_coordinator_config_t config = {
    .provisioning_timeout_ms = 120000U,
    .provisioning_connection_grace_ms = 30000U,
    .provisioning_poll_period_ms = 200U,
    .provisioning_max_sessions = 3U,
    .provisioning_retry_backoff_ms = 1500U,
    .provisioning_failure_dwell_ms = 1000U,
};
```

All values must be non-zero. The session count is bounded to 10 and timing
values to 10 minutes; every delay must convert to at least one FreeRTOS tick.
The coordinator copies this structure during initialization.
`provisioning_timeout_ms` bounds the normal session. The
`provisioning_connection_grace_ms` window starts only if that session deadline
is reached while a credential handoff is still pending. With the values above,
the ordinary credential wait lasts at most 150 seconds. Only a handoff still
pending at that boundary can then enter clean teardown and the fixed five-second
late-DHCP settle.

## Late DHCP Recovery

ESP-NETIF remains the DHCP lifecycle owner; neither the coordinator nor
`wifi_manager` manually starts or stops DHCP. At the 120-second session
deadline the coordinator reads the legacy boolean handoff state; a pending
handoff gets the configured 30-second grace.

Observed hardware logs show a DHCP OFFER/ACK can arrive only after BLE and the
provisioning framework stop. Starting the next BLE session immediately then
calls into Wi-Fi setup and cancels that valid lease. To close this boundary,
grace expiry performs a non-blocking verified queue receive, claims Phase 7
reset exclusion, receives once more, and arms only the active generation.
Normal cleanup reaches `STOPPED` while the manager preserves only that armed
RAM copy. A final queue receive catches framework success that won during
teardown.

If the queue remains empty, the coordinator polls `wifi_manager` for no more
than five seconds. Only `CONNECTED`, `has_ipv4_address`, and an exact connected
SSID match allow `provisioning_manager` to promote the retained credential to
the verified queue. The existing persistence/read-back/adoption flow is then
reused unchanged. No password is exposed for the SSID comparison and no
credential is persisted before verification.

Reset, timeout, disconnect, mismatch, or API failure drains and zeroizes all
late copies. A non-reset failure requests unmanaged Station detach after clean
`STOPPED` and before another session begins. `WIFI_CREDENTIAL_FAILED` remains a
non-terminal progress update at the coordinator boundary.

Success still follows the normal security and ownership path:

```text
verified credential queue
    -> config_manager persistence and read-back
    -> BLE stop/deinit
    -> wifi_manager adoption
    -> coordinator ONLINE runtime tracking
    -> cloud start gate opens
    -> provisioning SUCCESS for 1500 ms
    -> WIFI_STATUS
    -> best-effort terminal BLE memory release
```

The coordinator never promotes a late Wi-Fi event directly from
`PROVISIONING` or `FAILED` to `ONLINE`.

## Phase 6.3 Acceptance

Checkpoint 6.3.2 is complete and was hardware-accepted on 2026-07-26. The
accepted path covers provisioning timeout, reset, successful reprovisioning,
Wi-Fi adoption, deferred cloud task startup, Firebase upload recovery, and GUI
cloud-state updates. This checkpoint does not complete the remaining Phase 6.3
work.

Checkpoint 6.3.3 is complete and was hardware-accepted on 2026-07-26. Runtime
Wi-Fi snapshots now update the coordinator between `CONNECTING`, `ONLINE`, and
`OFFLINE` without transferring reconnect ownership away from `wifi_manager` or
calling LVGL from the callback.

Checkpoint 6.3.4 separates local-service startup from network-service task
startup. It is implemented and build-verified, but hardware smoke tests have
not yet confirmed sensor/GUI responsiveness during provisioning, timeout and
watchdog behavior, late-DHCP recovery, reconnect, or Firebase upload recovery.

## Phase 6.4.1 Integration

Application screen orchestration is implemented and build validation is
tracked with the `app_gui` component. Hardware testing is still required.
This integration posts only screen commands and does not change config
storage, BLE lifecycle, Wi-Fi reconnect behavior, or provisioning success UI.
Later Phase 6.4 checkpoints add provisioning success, retry, and cloud
recovery without changing this routing ownership.

## Phase 6.4.3 Integration

**IMPLEMENTED / HARDWARE TEST PENDING**

After `provisioning_manager_start()` succeeds, the coordinator obtains the
payload for that exact active service and posts it to `app_gui`. It preserves
the existing bounded credential wait, persistence, BLE cleanup, Wi-Fi
adoption, and screen-routing behavior.

## Phase 6.4.4 Integration

**IMPLEMENTED / HARDWARE TEST PENDING**

At the Phase 6.4.4 checkpoint the coordinator published the real provisioning
lifecycle and preserved the QR payload across ordinary screen transitions.
The 2026-08-01 selective restore reinstates its non-terminal credential-failure
mapping and session/grace ordering from `e66adb3`; target testing must determine
how the upstream service behaves after exhausting connection attempts. Verified
success persists and re-reads configuration,
waits for manager cleanup, adopts the valid Station/IPv4 connection, shows
`SUCCESS` for 1500 ms, then requests `WIFI_STATUS`. The existing Wi-Fi-screen
timer continues to route to `SENSOR_DASHBOARD`. Coordinator runtime tracking
begins at adoption, but normal `ONLINE` screen requests are suppressed during
the success dwell so a reconnect cannot cut the success presentation short.

## Phase 6.4.5 Integration

**IMPLEMENTED / HARDWARE TEST PENDING**

The existing coordinator task now contains an outer bounded retry loop and a
one-session function. Callback registration occurs once before the loop. Each
session gets the next non-zero `uint32_t` generation; zero is reserved and is
skipped if increment wraps. Manager progress is accepted only when its
generation matches the coordinator's active generation. Every GUI status also
carries the one-based session number and configured session limit, allowing
the QR screen to show `Session n/max` throughout waiting, cleanup, retry, and
success states.

Before a replacement session, cleanup must reach `STOPPED`, handoff-pending
must be false, and `config_manager` must still report `NOT_CONFIGURED`. The
manager resets its retained credential queue during reinitialization. If
configuration becomes `VALID`, no new BLE service starts; the coordinator uses
the stored-configuration path instead. Intermediate retries retain BLE
controller memory. Success, final exhaustion, and nonretryable clean stops
attempt release once after the complete retry envelope. Release is
best-effort: it cannot prevent adoption, replace the existing
timeout/storage/adoption result, overwrite terminal UI state, or close the
cloud start gate. On success, the established 1500 ms dwell and `WIFI_STATUS`
route finish before the release diagnostic is evaluated. The cloud gate opens
at coordinator `ONLINE` after provisioning framework cleanup and connection
adoption; it intentionally does not depend on optional BLE memory reclamation.

| Result class | Examples | New phone session |
|---|---|---|
| `SUCCESS` | Persisted, cleaned, connection adopted | No |
| `RETRYABLE_TIMEOUT` | Idle session expired, or the bounded grace/late settle produced no verified handoff | Yes, while budget remains |
| `RETRYABLE_SESSION_FAILURE` | Session stopped cleanly without a usable handoff, including late reconciliation failure | Yes, while budget remains |
| `NONRETRYABLE_STORAGE_FAILURE` | Save, validation, or read-back failure | No |
| `NONRETRYABLE_ADOPTION_FAILURE` | Persisted connection cannot be adopted | No |
| `NONRETRYABLE_INTERNAL_FAILURE` | Init/start/queue or cleanup cannot be safely completed | No |

A wrong-password result displays `FAILED` but is not independently promoted to
a terminal coordinator error. This intentionally restores the `e66adb3`
ordering. Session timeout or a clean manager stop still drives the outer
cleanup/retry envelope. On final exhaustion the QR is cleared, final `TIMEOUT`
or `FAILED` remains on `PROVISIONING`, coordinator state becomes `FAILED`, and
cloud startup remains gated.

### Phase 6.4.5 Manual Hardware Tests

- **Test A — Timeout then automatic retry:** leave session 1 idle; verify
  `TIMEOUT -> CLEANING_UP -> RETRYING -> STARTING -> WAITING_FOR_PHONE`, no
  reboot, manager `STOPPED` before restart, discoverable session 2, a new QR
  model, and no heap drop.
- **Test B — Wrong then correct password:** submit wrong credentials; verify
  `FAILED` without an application-forced immediate generation change. Verify
  timeout/manager cleanup remains bounded before any replacement session, then
  submit corrected credentials and verify the success flow.
- **Test C — Retry exhaustion:** allow all three default sessions to timeout;
  verify exactly three sessions, no fourth service, final QR cleared, terminal
  state remains visible, Station is disconnected, no reboot, and no cloud
  start.
- **Test D — Successful second session:** timeout session 1 and provision
  session 2; verify persistence/read-back, cleanup, adoption, about 1500 ms
  `SUCCESS`, then `WIFI_STATUS` and `SENSOR_DASHBOARD`; reboot must use stored
  credentials.
- **Test E — Stale event injection:** deliver generation N status/QR clear
  after N+1 is active; verify the old message is ignored, the new QR remains,
  and `WAITING_FOR_PHONE` is not overwritten.
- **Test F — NVS save failure:** inject `config_manager_save_wifi()` failure;
  verify `FAILED`, no new session, no adoption/cloud/success, and temporary
  credentials are cleared.
- **Test G — Cleanup timeout:** prevent manager `STOPPED`; verify no retry, no
  duplicate BLE manager, final `FAILED`, QR cleared, and bounded exit.
- **Test H — Runtime stability:** repeat timeout/retry cycles for 30–60
  minutes; verify no watchdog, LVGL assertion, stack warning, duplicate
  callback/queue, continuous heap reduction, or BLE/NimBLE leak.

## Future Attention

- Add cancellation or runtime reprovisioning APIs only when separately
  approved.
- Complete Phase 7.5 hardware race testing before accepting reset safety in
  every provisioning lifecycle state.

## Phase 6.4.6 Integration

**IMPLEMENTED / HARDWARE TEST PENDING**

The coordinator preflight now treats ESP32-S3 BLE reclamation as optional
terminal cleanup. The existing Wi-Fi callback remains the only runtime
callback; `main` fans its copied IPv4 state to the coordinator, GUI, and cloud
network epoch. The cloud task still cannot start in `PROVISIONING` or between
Phase 6.4.5 sessions, and opens only at stored-connection `CONNECTING` or
post-adoption `ONLINE`.

## Phase 6.4.7 Closure Status

**IMPLEMENTED / HARDWARE REGRESSION PENDING**

Static closure review confirms that the coordinator alone owns config-state
classification, the bounded outer provisioning retry policy, persistence
verification, cleanup waiting, active-connection adoption, and application
network state. It registers the manager progress callback once, rejects stale
generations, requires `STOPPED` before retry, and never changes a valid runtime
disconnect into BLE reprovisioning. UI publication and terminal BLE release
remain best-effort diagnostics and cannot replace the primary network result.

Configured boot requests `BOOT`, connects stored credentials, then follows
valid IPv4 through `WIFI_STATUS` to the dashboard. `NOT_CONFIGURED` requests
`PROVISIONING` directly. Integrity failures are preserved on the safe boot
path without erasing configuration or starting cloud. The complete A-N
hardware regression matrix is maintained in the project roadmap.
