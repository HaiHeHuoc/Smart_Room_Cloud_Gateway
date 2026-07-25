# Provisioning Manager

## Purpose

`provisioning_manager` owns the temporary BLE provisioning transport lifecycle.
It wraps the Espressif `network_provisioning` component and exposes a small,
thread-safe API for initialization, service startup, asynchronous shutdown,
cleanup, and state inspection.

Phase 6.1 is complete. This phase verifies BLE bring-up and resource cleanup;
it does not yet accept, validate, persist, or apply Wi-Fi credentials.

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
- Requests provisioning shutdown asynchronously.
- De-initializes the framework after `NETWORK_PROV_END`.
- Releases BTDM resources through
  `NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM`.
- Exposes a thread-safe state snapshot for application-side polling.

## Dependencies

| Dependency | Use |
|---|---|
| `esp_common` | Public `esp_err_t` API |
| `esp_hw_support` | Station MAC address |
| `freertos` | State spinlock and one-shot cleanup task |
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
- `STOPPED` is terminal for the current Phase 6.1 boot flow.
- Initialization, startup, cleanup-task creation, or de-initialization errors
  enter `FAILED`.

## Threading And Cleanup

The component protects only its small lifecycle state with `s_state_lock`.
No BLE, Wi-Fi, logging, task-creation, or framework API is called while that
spinlock is held.

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

/* Provisioning remains active until application policy requests shutdown. */

ret = provisioning_manager_stop();
if (ret != ESP_OK) {
    return ret;
}

provisioning_manager_state_t state;
ret = provisioning_manager_get_state(&state);
```

The caller should poll with a finite delay and timeout. A tight polling loop
would waste CPU and flood the state log.

## Current Phase 6.1 Integration

The temporary bring-up path in `main.c`:

1. Initializes Wi-Fi infrastructure and registers the Wi-Fi status callback.
2. Initializes and starts `provisioning_manager`.
3. Leaves BLE advertising active for the configured bring-up interval.
4. Requests stop and polls for `STOPPED` with a finite timeout.
5. Holds the application in the isolated bring-up path.

Normal sensor, Firebase, cloud, and production Wi-Fi startup intentionally do
not resume from this temporary path.

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
- BLE stop and framework cleanup.

This component does not own:

- Application NVS schema or persistent Wi-Fi configuration.
- Credential validation or handoff to `config_manager`.
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

- Consume the safe Wi-Fi credential event without logging sensitive data.
- Validate and copy credentials before leaving callback context.
- Persist credentials only through `config_manager`.
- Re-read configuration state and continue only when it is `VALID`.
- Let `wifi_manager` initiate Station connection after the copied handoff.
- Define retry, client-disconnect, and cancellation behavior.
- Replace the development Proof of Possession with a production strategy.
- Add focused lifecycle and hardware tests for later Phase 6 work.
