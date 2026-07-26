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
- Persists provisioned credentials through `config_manager`.
- Verifies NVS state and credential read-back.
- Waits for provisioning cleanup and asks `wifi_manager` to adopt the active
  connection.
- Tracks the application-level network boot state.

The coordinator does not:

- Access NVS directly.
- Own Wi-Fi event handlers or reconnect behavior.
- Own BLE transport internals.
- Call LVGL or GUI APIs.
- Log passwords, PoP values, tokens, or credential contents.

## Public API

| API | Responsibility |
|---|---|
| `app_network_coordinator_init()` | Copy timing configuration and enter `READY`. |
| `app_network_coordinator_start()` | Schedule the one-shot coordinator task and return immediately. |
| `app_network_coordinator_get_state()` | Copy the thread-safe lifecycle state. |
| `app_network_coordinator_state_to_string()` | Convert a state to readable text. |

## State Flow

```text
UNINITIALIZED
    -> READY
    -> STARTING
    -> RESOLVING_CONFIG
       -> CONNECTING
       -> PROVISIONING -> ONLINE
       -> FAILED
```

`CONNECTING` represents an asynchronous stored-credential connection request.
`ONLINE` is currently assigned after the provisioning connection is verified,
cleaned up, and adopted.

`app_network_coordinator_start()` does not wait for provisioning. The dedicated
task performs the bounded receive, persistence, cleanup, and adoption flow so
`app_main()` can continue initializing independent services.

The composition root may initialize dependent service state immediately, then
defer memory-heavy tasks until the coordinator reaches `CONNECTING` for a
stored connection or `ONLINE` after provisioning cleanup and adoption.

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

## Threading And Security

- Lifecycle state and copied configuration are protected by a short critical
  section.
- The one-shot coordinator task uses a 6 KB stack at priority 4 and deletes
  itself after boot orchestration succeeds or fails.
- Manager, NVS, Wi-Fi, logging, and callback APIs are never called while that
  critical section is held.
- Provisioning waits use configured finite timeout and poll periods.
- Temporary SSID/password buffers are securely overwritten on all completed
  paths.
- GUI updates remain event-driven through the registered `wifi_manager`
  callback and `app_gui` queue.

## Configuration

```c
static const app_network_coordinator_config_t config = {
    .provisioning_timeout_ms = 120000U,
    .provisioning_poll_period_ms = 200U,
};
```

Both timing values must be greater than zero. The coordinator copies this
structure during initialization.

## Phase 6.3.2 Acceptance

Checkpoint 6.3.2 is complete and was hardware-accepted on 2026-07-26. The
accepted path covers provisioning timeout, reset, successful reprovisioning,
Wi-Fi adoption, deferred cloud task startup, Firebase upload recovery, and GUI
cloud-state updates. This checkpoint does not complete the remaining Phase 6.3
work.

## Future Attention

- Add an event-driven coordinator update path if application code needs its
  state to follow later disconnect and reconnect events.
- Add cancellation or restart APIs only when runtime reprovisioning policy is
  approved.
- Keep factory reset outside this component until Sprint 7 ownership is
  finalized.
