# Provisioning Manager

## Purpose

`provisioning_manager` owns the temporary BLE Wi-Fi provisioning transport.
It wraps Espressif `network_provisioning`, validates and deep-copies received
credentials, releases BLE resources asynchronously, and exposes verified
credentials without owning persistent storage or reconnect policy.

Phase 6.2 is complete. The application starts this component only when
`config_manager` reports `NOT_CONFIGURED`. Phase 6.4.3 adds a bounded copy API
for the QR payload of the service that is actually active. Phase 6.4.4 adds a
single copied progress callback for application orchestration. Phase 6.4.5
adds clean same-boot `STOPPED -> READY` reinitialization and session-generation
metadata for bounded application retries.

## Component Structure

```text
components/connectivity/provisioning_manager/
|-- CMakeLists.txt
|-- provisioning_manager.c
|-- include/
|   `-- provisioning_manager.h
`-- docs/
    `-- README.md
```

## Implemented Behavior

- Initializes Espressif Network Provisioning Manager with the BLE scheme.
- Uses Security 1 with a temporary development Proof of Possession.
- Builds the BLE service name as `PROV_` plus the last three Station MAC bytes.
- Matches the official `wifi_prov` example's five framework connection
  attempts for each received credential set.
- Builds the official Security 1 QR JSON only after the BLE service starts.
- Caches the exact active service identity and exposes only a caller-owned QR
  payload copy while the service remains `ACTIVE`.
- Starts BLE provisioning only from the `READY` state.
- Reinitializes from either `UNINITIALIZED` or a fully cleaned `STOPPED` state
  for a new non-zero session generation.
- Reuses one retained credential queue and calls `xQueueReset()` before each
  replacement session instead of allocating a queue per retry.
- Prevents concurrent callers from claiming the same lifecycle transition.
- Deep-copies and validates framework-owned credentials on receipt.
- Keeps credentials pending until the framework reports Wi-Fi success.
- Exposes a thread-safe, non-sensitive handoff-pending snapshot so application
  policy can distinguish idle provisioning from an in-flight Wi-Fi attempt.
- Discards pending credentials after a failed connection attempt.
- Delivers one verified copy through a length-one FreeRTOS queue.
- Publishes non-sensitive `STARTING`, waiting, credential, Wi-Fi result,
  stopping, stopped, and terminal-failure progress snapshots with the exact
  session generation that produced each event.
- Invokes the single registered progress callback outside the state critical
  section; the callback never receives credentials, QR JSON, or a manager-owned
  pointer.
- Requests provisioning shutdown asynchronously.
- De-initializes the framework after `NETWORK_PROV_END`.
- Retains BLE controller memory across intermediate clean stops and releases
  it through an explicit terminal API after the retry envelope ends.
- Exposes a thread-safe state snapshot for application-side polling.
- Clears temporary credential copies and never logs passwords or PoP.

## Dependencies

| Dependency | Use |
|---|---|
| `esp_common` | Public `esp_err_t` API |
| `bt` | ESP32-S3 BLE memory retention and terminal release |
| `esp_hw_support` | Station MAC address |
| `esp_wifi` | Framework Wi-Fi credential type |
| `freertos` | Queue, state spinlock, and one-shot cleanup task |
| `log` | Lifecycle and error logging |
| `network_provisioning` | BLE transport and provisioning framework |

The application manifest currently selects
`espressif/network_provisioning` version `^1.2.4`.

## Public API

| API | Responsibility |
|---|---|
| `provisioning_manager_register_progress_callback()` | Register, idempotently retain, or unregister the single copied progress callback |
| `provisioning_manager_init(generation)` | Initialize or cleanly reinitialize the BLE scheme and enter `READY` |
| `provisioning_manager_start()` | Start advertising and enter `ACTIVE` |
| `provisioning_manager_get_qr_payload(generation, ...)` | Copy the exact generation's active BLE QR JSON |
| `provisioning_manager_stop()` | Begin asynchronous shutdown |
| `provisioning_manager_release_ble_memory()` | Permanently release retained BLE memory after the retry envelope |
| `provisioning_manager_get_state()` | Copy the current lifecycle state |
| `provisioning_manager_is_wifi_handoff_pending()` | Report whether received credentials are still connecting or awaiting application consumption |
| `provisioning_manager_receive_wifi_credentials()` | Wait for credentials from a framework-confirmed connection |

All public APIs return `esp_err_t`. The component does not call
`ESP_ERROR_CHECK()` and leaves policy decisions to the application.
Generation zero is reserved. `provisioning_manager_get_qr_payload()` requires a
`PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE` buffer, never returns an internal
pointer, and returns `ESP_ERR_INVALID_STATE` outside the matching active
session.

## Espressif wifi_prov Alignment

The implementation follows
`managed_components/espressif__network_provisioning/examples/wifi_prov` for
the interoperable provisioning contract:

- BLE transport through `network_prov_scheme_ble`;
- NimBLE controller configuration;
- Security 1 with Proof of Possession;
- five framework Wi-Fi connection attempts;
- QR version `v1` and transport `ble`;
- QR JSON schema:

```json
{"ver":"v1","name":"PROV_XXXXXX","pop":"<development-pop>","transport":"ble"}
```

The application intentionally does not copy the example's global event
handler, direct `esp_wifi_connect()` call, credential logging, custom endpoint,
or provisioning-state reset callback. It also replaces the example's immediate
`FREE_BTDM` policy with `NETWORK_PROV_EVENT_HANDLER_NONE`, retaining BLE across
the bounded same-boot retry envelope. After the framework is fully
deinitialized and no later session is allowed, the application explicitly
releases the ESP32-S3 BLE-only allocation with
`esp_bt_mem_release(ESP_BT_MODE_BLE)`. Those behaviors preserve the existing
`wifi_manager`, coordinator, security, and Phase 6.4 ownership boundaries.

## State Lifecycle

```text
UNINITIALIZED -> READY -> STARTING -> ACTIVE -> STOPPING -> STOPPED
                   ^                                      |
                   +--------- next generation ------------+
                     \
                      +-------------------------------> FAILED
```

- `READY -> STARTING` and `ACTIVE -> STOPPING` are claimed atomically.
- `STOPPING` covers BLE transport shutdown and framework de-initialization.
- `STOPPED` is a clean barrier: framework deinitialization and the cleanup task
  have completed, so the next generation may re-enter `READY`.
- Initialization, startup, cleanup-task creation, or de-initialization errors
  enter `FAILED`.
- `FAILED` is not silently reset. A new session is forbidden unless the
  previous session reached `STOPPED`.

## Credential Handoff

```text
NETWORK_PROV_WIFI_CRED_RECV
    -> validate and copy to pending storage
    -> mark Wi-Fi handoff pending
    -> wait for the framework connection result

NETWORK_PROV_WIFI_CRED_FAIL
    -> clear pending credentials
    -> clear handoff-pending state

NETWORK_PROV_WIFI_CRED_SUCCESS
    -> move the pending copy into the handoff queue
    -> application persists through config_manager
    -> clear handoff-pending state after application consumption
```

The queue holds an independent copy. The caller must clear its output after
persistence or on every error path. The component never calls
`config_manager`, `wifi_manager`, GUI, cloud, or reboot APIs from its callback.
The handoff-pending API returns only a boolean. Progress snapshots contain only
a non-zero session generation, an enum, `esp_err_t`, and the framework's
non-sensitive Wi-Fi failure reason; they never expose SSID or password
contents.

## Timeout Boundary Handling

The credential receive API still honors each caller-supplied finite timeout.
The application coordinator first waits for the normal provisioning session
deadline. If that deadline expires while the handoff-pending snapshot is true,
the coordinator performs one additional bounded connection wait. This catches
a `GOT_IP` event arriving near the deadline without accepting unverified
credentials or treating an unadopted Station connection as application-ready.

## Threading And Cleanup

The component protects only lifecycle state and short pending-credential copy
operations with `s_state_lock`. No BLE, Wi-Fi, logging, queue, task-creation,
or framework API is called while that spinlock is held.

The progress callback and its context are copied while the lock is held, then
the callback is invoked after the lock is released. The callback runs in normal
task context, must return promptly, must not retain its snapshot pointer, and
must not call LVGL.

The upstream framework invokes the direct `NETWORK_PROV_END` callback while it
still owns an internal mutex. Calling `network_prov_mgr_deinit()` directly from
that callback deadlocks. The component therefore creates a one-shot
`prov_cleanup` task with a 4 KiB stack and priority 4. The callback returns,
the framework releases its mutex, and the cleanup task safely performs
de-initialization. State remains `STOPPING` during `NETWORK_PROV_DEINIT`; only
the cleanup task, after `network_prov_mgr_deinit()` returns, publishes
`STOPPED`. This prevents a new session from racing a logically active cleanup
task.

If the cleanup task cannot be created, the component enters `FAILED` instead
of remaining indefinitely in `STOPPING`.

If BLE service startup fails, framework deinitialization is still attempted.
A cleanup failure leaves the manager in `FAILED` and blocks reuse, while the
API and terminal progress retain the original service-start error as the
primary failure. The cleanup error remains available in a separate diagnostic
log.

The progress callback registration and credential queue survive a clean stop.
The next `init(generation)` clears pending credentials and handoff flags,
resets the queue, clears active identity, and initializes the framework again.
Each asynchronous terminal event uses a captured generation so a late old
`STOPPED` callback cannot be relabeled as the new session.

The ESP32-S3 target is configured for NimBLE/BLE rather than Classic
Bluetooth. No Classic-BT release is attempted at session initialization. BLE
controller/host memory is retained across intermediate retries, then
`provisioning_manager_release_ble_memory()` calls
`esp_bt_mem_release(ESP_BT_MODE_BLE)` only from clean `STOPPED` after the full
retry envelope terminates. The call is idempotent:
`ESP_ERR_NOT_FOUND` is accepted as already released; other release failures
increment a diagnostic counter and remain visible as warnings.

Terminal memory reclamation is best-effort application cleanup. It cannot
replace a storage, adoption, timeout, or configuration result. In particular,
successful provisioning first verifies IPv4, adopts the Station connection,
sets coordinator `ONLINE`, publishes `SUCCESS`, preserves the 1500 ms dwell
and screen route, and only then attempts BLE memory release.

The active service name and QR payload are cleared when stop begins and on
startup, cleanup, or de-initialization failure. QR construction failure does
not stop an otherwise valid BLE session; the coordinator reports the
best-effort GUI publication failure and provisioning can still continue
manually.

## Basic Usage

```c
esp_err_t ret = provisioning_manager_register_progress_callback(
    application_progress_callback,
    NULL);

const uint32_t generation = 1U;

ret = provisioning_manager_init(generation);
if (ret != ESP_OK) {
    return ret;
}

ret = provisioning_manager_start();
if (ret != ESP_OK) {
    return ret;
}

char qr_payload[PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE];
ret = provisioning_manager_get_qr_payload(
    generation,
    qr_payload,
    sizeof(qr_payload));
/* Copy to the UI queue, then securely clear this caller-owned buffer. */

provisioning_manager_wifi_credentials_t credentials;
ret = provisioning_manager_receive_wifi_credentials(&credentials, 120000U);

ret = provisioning_manager_stop();
if (ret != ESP_OK) {
    return ret;
}

provisioning_manager_state_t state;
ret = provisioning_manager_get_state(&state);

/* Call only when no later same-boot provisioning session is allowed. */
ret = provisioning_manager_release_ble_memory();
/* Log ret as cleanup diagnostics; do not replace the network result. */
```

The caller should poll with a finite delay and timeout. A tight polling loop
would waste CPU and flood the state log.

## Production Integration

The production provisioning path in `main.c`:

1. Starts provisioning only when Wi-Fi state is `NOT_CONFIGURED`.
2. Waits with a finite session timeout for framework-verified credentials.
3. Allows one bounded connection grace only when credentials are already
   pending at the session deadline.
4. Persists only through `config_manager`.
5. Re-reads state and data; continues only after successful verification.
6. Waits for BLE cleanup and the active Station connection.
7. Lets `wifi_manager` adopt the connection and own later Wi-Fi events.
8. Clears every application credential copy.
9. Maps manager progress to the GUI through coordinator-owned latest-value
   status publication.

Integrity states such as `INCOMPLETE`, `INVALID_DATA`, and
`UNSUPPORTED_VERSION` are preserved and do not automatically start
provisioning.

## Security Notes

- The current Proof of Possession is a development value compiled into
  firmware. It is not a production credential.
- The QR payload contains that PoP. It is copied only into bounded transient
  buffers and must never be logged.
- The upstream `security1` log tag is reduced to `WARN` before a session so
  normal serial output does not print session key material.
- Production devices should use a device-specific value obtained from
  protected configuration or manufacturing data.
- Do not log or document real Proofs of Possession, SSIDs, passwords, tokens,
  private keys, or administrator credentials.
- The component logs the BLE service name but never logs Wi-Fi credentials.

## Ownership Boundaries

This component owns:

- BLE provisioning scheme lifecycle.
- Provisioning service naming and current Security 1 setup.
- Exact active QR identity and payload construction.
- Lifecycle state synchronization.
- Transient validation and credential handoff after connection success.
- BLE stop and framework cleanup.

This component does not own:

- Application NVS schema or persistent Wi-Fi configuration.
- Persistent credential writes or direct calls to `config_manager`.
- Station reconnect policy owned by `wifi_manager`.
- GUI, sensor, cloud, Firebase, factory-reset, or reboot policy.

## Expected Bring-Up Logs

```text
I (...) PROVISIONING_MANAGER: Provisioning manager initialized
I (...) PROVISIONING_MANAGER: BLE provisioning active with service name: PROV_XXXXXX
I (...) APP_NETWORK_COORDINATOR: Active provisioning QR payload queued for GUI
I (...) PROVISIONING_MANAGER: Provisioning stop requested
I (...) PROVISIONING_MANAGER: Provisioning service stopped
I (...) PROVISIONING_MANAGER: Provisioning manager de-initialized
```

## Future Attention

- Replace the development Proof of Possession with a production strategy.
- Add factory-reset and reprovisioning policy in Sprint 7.

## Phase 6.4.6 BLE Release Correction

**IMPLEMENTED / HARDWARE TEST PENDING**

Static inspection of ESP-IDF 6.0.1, the ESP32-S3 target configuration, and the
installed `network_provisioning` BLE scheme confirms the terminal release mode
is `ESP_BT_MODE_BLE`. Hardware tests must still verify successful release,
already-released handling, injected release failure, multi-session retention,
and continued Wi-Fi/cloud success when optional reclamation fails.
- Add focused automated tests around callback ordering where the upstream
  framework can be isolated.

## Phase 6.2 Verification

- BLE credential transfer and successful Station connection were exercised on
  hardware.
- Credential persistence and read-back validation were exercised.
- The `6.2.3B-4` injected NVS persistence failure and retry recovery test
  passed on hardware.
- The temporary fault-injection code was removed after acceptance.

## Phase 6.4.3 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

The QR contract, active-session copy API, five framework connection attempts,
NimBLE/Security1 defaults, and build have been verified statically. The user
confirmed a phone QR scan and successful provisioned Wi-Fi connection on the
target. Full callback ordering, persistence/reboot, replacement-session,
cleanup, and endurance coverage remains in the final A-N hardware matrix.

## Phase 6.4.4 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

The manager now exposes the copied lifecycle facts required by the coordinator.
Normal credential failure remains non-terminal for the current BLE session;
terminal manager failures and stop/deinit invalidate the active session
identity. Build validation passed, while callback ordering, retry-within-session,
timeout, and successful cleanup still require target-hardware acceptance.

## Phase 6.4.5 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

The manager now supports clean same-boot reinitialization from `STOPPED`,
retains and resets one credential queue, preserves the single callback
registration, attaches a non-zero generation to progress and QR snapshots,
keeps `STOPPING` until the cleanup task has fully returned, and retains BLE
memory only until the bounded retry envelope ends. Hardware must still verify
that replacement BLE sessions advertise without reboot and repeated cycles do
not leak controller, queue, or credential resources.

## Phase 6.4.7 Closure Status

**IMPLEMENTED / HARDWARE REGRESSION PENDING**

Static closure review confirms one retained credential queue, one progress
callback, generation-bound QR/progress data, `STOPPED` before replacement
sessions, credential zeroization, and BLE retention between retries. The
Phase 6.4 final hardware matrix in the project roadmap remains the acceptance
authority; no additional provisioning feature is introduced here.
