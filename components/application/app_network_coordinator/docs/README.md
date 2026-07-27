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
- Copies the exact active BLE QR payload into the GUI's dedicated latest-value
  queue immediately after provisioning starts.
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
  asynchronous `app_gui` screen requests and copied QR models.
- Log passwords, PoP values, tokens, or credential contents.

## Public API

| API | Responsibility |
|---|---|
| `app_network_coordinator_init()` | Copy timing configuration and enter `READY`. |
| `app_network_coordinator_start()` | Schedule the one-shot coordinator task and return immediately. |
| `app_network_coordinator_get_state()` | Copy the thread-safe lifecycle state. |
| `app_network_coordinator_state_to_string()` | Convert a state to readable text. |
| `app_network_coordinator_notify_wifi_event()` | Apply a short runtime Wi-Fi state notification after normal Station ownership begins. |

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
Provisioning events remain ignored until credentials are persisted, BLE is
cleaned up, and the active connection is adopted.

For an unconfigured device, startup ordering is:

```text
request PROVISIONING screen
    -> initialize/start BLE service
    -> copy active QR payload from provisioning_manager
    -> post copied payload to app_gui
    -> wait for framework-verified credentials
```

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
- Inspection failures and non-provisionable integrity states make a
  best-effort `BOOT` request before returning the existing policy error.
- A normal stored-credential `CONNECTING -> ONLINE` transition queues
  `WIFI_STATUS`.
- Transient provisioning `GOT_IP` events remain ignored for screen routing.

## Threading And Security

- Lifecycle state and copied configuration are protected by a short critical
  section.
- The one-shot coordinator task uses a 6 KB stack at priority 4 and deletes
  itself after boot orchestration succeeds or fails.
- Manager, NVS, Wi-Fi, logging, and callback APIs are never called while that
  critical section is held.
- `app_network_coordinator_notify_wifi_event()` performs no allocation,
  blocking wait, Wi-Fi call, or LVGL call. A verified normal transition to
  `ONLINE` may post one non-blocking `WIFI_STATUS` screen request, so the API
  remains suitable for the normal task-context Wi-Fi status callback.
- Provisioning waits use configured finite timeout and poll periods.
- A session timeout receives one additional finite connection grace only when
  `provisioning_manager` reports that valid credentials are already in flight.
- Temporary SSID/password buffers are securely overwritten on all completed
  paths.
- The temporary QR copy is overwritten immediately after its non-blocking GUI
  post and its contents are never logged.
- GUI status updates remain event-driven through the registered
  `wifi_manager` callback. Config-driven initial routing and verified normal
  `ONLINE` routing use the independent `app_gui` command queue.

## Configuration

```c
static const app_network_coordinator_config_t config = {
    .provisioning_timeout_ms = 120000U,
    .provisioning_connection_grace_ms = 30000U,
    .provisioning_poll_period_ms = 200U,
};
```

All timing values must be greater than zero. The coordinator copies this
structure during initialization. The 30-second grace is not added when no
credential handoff is pending, so an idle provisioning session still stops at
its normal deadline.

## Late DHCP Recovery

When the normal provisioning deadline and a framework `GOT_IP` event race,
the coordinator checks only the non-sensitive handoff-pending snapshot. A
pending handoff receives one bounded grace wait. Success still follows the
normal security and ownership path:

```text
verified credential queue
    -> config_manager persistence and read-back
    -> BLE stop/deinit
    -> wifi_manager adoption
    -> coordinator ONLINE
    -> cloud start gate opens
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
Phase 6.4 remains incomplete.

## Phase 6.4.3 Integration

**IMPLEMENTED / HARDWARE TEST PENDING**

After `provisioning_manager_start()` succeeds, the coordinator obtains the
payload for that exact active service and posts it to `app_gui`. It preserves
the existing bounded credential wait, persistence, BLE cleanup, Wi-Fi
adoption, and screen-routing behavior. Phase 6.4.4 progress events and
success/failure screen transitions remain intentionally deferred.

## Future Attention

- Add cancellation or restart APIs only when runtime reprovisioning policy is
  approved.
- Keep factory reset outside this component until Sprint 7 ownership is
  finalized.
