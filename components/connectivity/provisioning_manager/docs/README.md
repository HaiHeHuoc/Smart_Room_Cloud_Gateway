# Provisioning Manager

## Purpose

`provisioning_manager` owns the temporary BLE Wi-Fi provisioning transport.
It wraps Espressif `network_provisioning`, validates and deep-copies received
credentials, releases BLE resources asynchronously, and exposes verified
credentials without owning persistent storage or reconnect policy.

Phase 6.2 is complete. The application starts this component only when
`config_manager` reports `NOT_CONFIGURED`.

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
- Starts BLE provisioning only from the `READY` state.
- Prevents concurrent callers from claiming the same lifecycle transition.
- Deep-copies and validates framework-owned credentials on receipt.
- Keeps credentials pending until the framework reports Wi-Fi success.
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
| `provisioning_manager_stop()` | Begin asynchronous shutdown |
| `provisioning_manager_get_state()` | Copy the current lifecycle state |
| `provisioning_manager_receive_wifi_credentials()` | Wait for credentials from a framework-confirmed connection |

All public APIs return `esp_err_t`. The component does not call
`ESP_ERROR_CHECK()` and leaves policy decisions to the application.

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
    -> wait for the framework connection result

NETWORK_PROV_WIFI_CRED_FAIL
    -> clear pending credentials

NETWORK_PROV_WIFI_CRED_SUCCESS
    -> move the pending copy into the handoff queue
    -> application persists through config_manager
```

The queue holds an independent copy. The caller must clear its output after
persistence or on every error path. The component never calls
`config_manager`, `wifi_manager`, GUI, cloud, or reboot APIs from its callback.

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
2. Waits with a finite timeout for framework-verified credentials.
3. Persists only through `config_manager`.
4. Re-reads state and data; continues only after successful verification.
5. Waits for BLE cleanup and the active Station connection.
6. Lets `wifi_manager` adopt the connection and own later Wi-Fi events.
7. Clears every application credential copy.

Integrity states such as `INCOMPLETE`, `INVALID_DATA`, and
`UNSUPPORTED_VERSION` are preserved and do not automatically start
provisioning.

## Security Notes

- The current Proof of Possession is a development value compiled into
  firmware. It is not a production credential.
- Production devices should use a device-specific value obtained from
  protected configuration or manufacturing data.
- Do not log or document real Proofs of Possession, SSIDs, passwords, tokens,
  private keys, or administrator credentials.
- The component logs the BLE service name but never logs Wi-Fi credentials.

## Ownership Boundaries

This component owns:

- BLE provisioning scheme lifecycle.
- Provisioning service naming and current Security 1 setup.
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
