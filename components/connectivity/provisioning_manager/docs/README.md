# Provisioning Manager

## Purpose

`provisioning_manager` owns the temporary BLE Wi-Fi provisioning transport.
It wraps Espressif `network_provisioning`, validates and deep-copies received
credentials, releases BLE resources asynchronously, and exposes verified
credentials without owning persistent storage or reconnect policy.

Phase 6.2 is complete. The application starts this component only when
`config_manager` reports `NOT_CONFIGURED`. Phase 6.4.3 adds a bounded copy API
for the QR payload of the service that is actually active.

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
- Prevents concurrent callers from claiming the same lifecycle transition.
- Deep-copies and validates framework-owned credentials on receipt.
- Keeps credentials pending until the framework reports Wi-Fi success.
- Exposes a thread-safe, non-sensitive handoff-pending snapshot so application
  policy can distinguish idle provisioning from an in-flight Wi-Fi attempt.
- Discards pending credentials after a failed connection attempt.
- Delivers one verified copy through a length-one FreeRTOS queue.
- Requests provisioning shutdown asynchronously.
- De-initializes the framework after `NETWORK_PROV_END`.
- Releases BTDM resources through
  `NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM`.
- Exposes a thread-safe state snapshot for application-side polling.
- Clears temporary credential copies and never logs passwords or PoP.

## Dependencies

| Dependency | Use |
|---|---|
| `esp_common` | Public `esp_err_t` API |
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
| `provisioning_manager_init()` | Initialize the BLE scheme and enter `READY` |
| `provisioning_manager_start()` | Start advertising and enter `ACTIVE` |
| `provisioning_manager_get_qr_payload()` | Copy the active BLE service's QR JSON into caller-owned storage |
| `provisioning_manager_stop()` | Begin asynchronous shutdown |
| `provisioning_manager_get_state()` | Copy the current lifecycle state |
| `provisioning_manager_is_wifi_handoff_pending()` | Report whether received credentials are still connecting or awaiting application consumption |
| `provisioning_manager_receive_wifi_credentials()` | Wait for credentials from a framework-confirmed connection |

All public APIs return `esp_err_t`. The component does not call
`ESP_ERROR_CHECK()` and leaves policy decisions to the application.
`provisioning_manager_get_qr_payload()` requires a
`PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE` buffer, never returns an internal
pointer, and returns `ESP_ERR_INVALID_STATE` outside an active session.

## Espressif wifi_prov Alignment

The implementation follows
`managed_components/espressif__network_provisioning/examples/wifi_prov` for
the interoperable provisioning contract:

- BLE transport through `network_prov_scheme_ble`;
- NimBLE controller configuration;
- Security 1 with Proof of Possession;
- `NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM`;
- five framework Wi-Fi connection attempts;
- QR version `v1` and transport `ble`;
- QR JSON schema:

```json
{"ver":"v1","name":"PROV_XXXXXX","pop":"<development-pop>","transport":"ble"}
```

The application intentionally does not copy the example's global event
handler, direct `esp_wifi_connect()` call, credential logging, custom endpoint,
or provisioning-state reset callback. Those behaviors would cross the
existing `wifi_manager`, coordinator, security, or Phase 6.4 ownership
boundaries.

## State Lifecycle

```text
UNINITIALIZED
    |
    v
READY -> STARTING -> ACTIVE -> STOPPING -> STOPPED
              \         \          \
               +---------+-----------> FAILED
```

- `READY -> STARTING` and `ACTIVE -> STOPPING` are claimed atomically.
- `STOPPING` covers BLE transport shutdown and framework de-initialization.
- `STOPPED` is terminal for the current boot flow.
- Initialization, startup, cleanup-task creation, or de-initialization errors
  enter `FAILED`.

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
The progress API returns only a boolean and never exposes SSID or password
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

The upstream framework invokes the direct `NETWORK_PROV_END` callback while it
still owns an internal mutex. Calling `network_prov_mgr_deinit()` directly from
that callback deadlocks. The component therefore creates a one-shot
`prov_cleanup` task with a 4 KiB stack and priority 4. The callback returns,
the framework releases its mutex, and the cleanup task safely performs
de-initialization. `NETWORK_PROV_DEINIT` then moves the component to `STOPPED`.

If the cleanup task cannot be created, the component enters `FAILED` instead
of remaining indefinitely in `STOPPING`.

The active service name and QR payload are cleared when stop begins and on
startup, cleanup, or de-initialization failure. QR construction failure does
not stop an otherwise valid BLE session; the coordinator reports the
best-effort GUI publication failure and provisioning can still continue
manually.

## Basic Usage

```c
esp_err_t ret = provisioning_manager_init();
if (ret != ESP_OK) {
    return ret;
}

ret = provisioning_manager_start();
if (ret != ESP_OK) {
    return ret;
}

char qr_payload[PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE];
ret = provisioning_manager_get_qr_payload(
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
```

The caller should poll with a finite delay and timeout. A tight polling loop
would waste CPU and flood the state log.

## Current Phase 6.2 Integration

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
- Add dedicated provisioning-state presentation in a later approved phase.
- Add factory-reset and reprovisioning policy in Sprint 7.
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
NimBLE/Security1 defaults, and build have been verified statically. A phone
scan, BLE handshake, Wi-Fi authentication, NVS persistence, and cleanup still
require target-hardware acceptance.
