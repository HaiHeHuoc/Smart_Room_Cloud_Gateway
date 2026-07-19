# Phase 5.3B - Wi-Fi Configuration Integrity and State Handling

## Status

Implementation review and build verification are complete for the component
scope described here. Hardware corruption and reboot-persistence tests remain
required before treating the wider Phase 5 as complete.

## Objective

Phase 5.3B ensures stored Wi-Fi credentials are classified before use. The
component must distinguish an unconfigured device from partial, unsupported,
or corrupt NVS data without exposing partial credentials to the caller.

This phase does not connect Wi-Fi or make application policy decisions.

## Stored Data

Namespace: `device_cfg`

| Key | Required type | Validation |
| --- | --- | --- |
| `cfg_ver` | `u32` | Must equal `CONFIG_MANAGER_CURRENT_VERSION` (`1`). |
| `wifi_ssid` | NVS string | Null-terminated, 1-32 bytes. |
| `wifi_pass` | NVS string | Empty, or null-terminated and 8-63 bytes. |

The integrity helper opens no handle and takes no mutex. Its caller must hold
the component mutex and pass an already-open `device_cfg` handle.

## Classification Truth Table

| Stored condition | State |
| --- | --- |
| Namespace is absent | `NOT_CONFIGURED` |
| No SSID and no password; version absent or retained | `NOT_CONFIGURED` |
| SSID only | `INCOMPLETE` |
| Password only | `INCOMPLETE` |
| SSID and password without version | `INCOMPLETE` |
| All keys, current version, valid values | `VALID` |
| All keys, non-current version | `UNSUPPORTED_VERSION` |
| Wrong NVS type for a required key | `INVALID_DATA` |
| SSID or password exceeds its destination buffer | `INVALID_DATA` |
| SSID is empty or password length is 1-7 bytes | `INVALID_DATA` |
| Mutex, open, or NVS access failure | `UNKNOWN` plus a non-OK API result |

The version-only case is intentionally `NOT_CONFIGURED` because
`config_manager_clear_wifi()` preserves `cfg_ver` while erasing both credential
keys.

## Public API Semantics

### `config_manager_get_wifi_config_state()`

This API reports integrity without returning SSID or password contents.

- It initializes the output to `UNKNOWN`.
- A missing namespace is a completed `NOT_CONFIGURED` classification and
  returns `ESP_OK`.
- Semantic problems return `ESP_OK` with a specific state.
- Storage or synchronization failures return their `esp_err_t` and leave the
  state as `UNKNOWN`.

### `config_manager_load_wifi()`

This API clears the caller output before accessing NVS. Credentials are copied
to the caller only for `VALID` data. Every failure leaves the output cleared.

| Integrity state | Returned error |
| --- | --- |
| `NOT_CONFIGURED` | `ESP_ERR_NVS_NOT_FOUND` |
| `VALID` | `ESP_OK` |
| `INCOMPLETE` | `ESP_ERR_INVALID_STATE` |
| `UNSUPPORTED_VERSION` | `ESP_ERR_NOT_SUPPORTED` |
| `INVALID_DATA` | `ESP_ERR_INVALID_RESPONSE` |

### `config_manager_has_wifi_config()`

This API returns `ESP_OK` with `false` only for `NOT_CONFIGURED`. It returns
`true` only when load succeeds. Incomplete, unsupported, invalid, mutex, and NVS
failures remain visible to the caller as errors.

### `config_manager_clear_wifi()`

This operation erases `wifi_ssid` and `wifi_pass`, preserves `cfg_ver`, and
commits only when at least one credential key existed. Repeating the operation
is safe and returns `ESP_OK`.

## Validation and Corruption Handling

NVS reads use bounded destination buffers. These outcomes are semantic
corruption rather than transport failures:

- `ESP_ERR_NVS_TYPE_MISMATCH` for any required Wi-Fi key.
- `ESP_ERR_NVS_INVALID_LENGTH` for an oversized SSID or password.
- A complete value that fails SSID or password length validation.

The private inspection helper converts these cases to `INVALID_DATA`. Other NVS
errors are preserved so callers can distinguish storage failure from corrupt
configuration.

## Resource and Concurrency Invariants

For load and state inspection:

```text
validate output
    -> lock
    -> open device_cfg read-only
    -> inspect keys
    -> close handle
    -> unlock
    -> zeroize temporary credentials
```

For save and clear:

```text
validate input
    -> lock
    -> open device_cfg read-write
    -> stage set/erase operations
    -> commit successful changes
    -> close handle
    -> unlock
    -> zeroize temporary credentials when present
```

No public operation retains an NVS handle. Every successful lock and open has a
matching unlock and close. Handles are always closed before the mutex is
released.

## Security Requirements

- Password contents must not appear in logs.
- Temporary credential snapshots are zeroized on success and failure paths.
- No Wi-Fi, GUI, cloud, provisioning, callback, or reboot API is called while
  the mutex is held.

## Review Findings Resolved

- Wi-Fi integrity inspection now reads `wifi_ssid` as well as version and
  password before classifying completeness.
- A retained version key after `clear_wifi()` no longer causes a false
  `INCOMPLETE` result.
- Load and state-inspection snapshots use explicit zeroization before return.
- Public enum values and state/error mappings now have Doxygen documentation.
- The component README now includes the Phase 5.3B API and state model.

## Acceptance Matrix

| Scenario | Expected result | Verification status |
| --- | --- | --- |
| Clean NVS | State `NOT_CONFIGURED`; load `ESP_ERR_NVS_NOT_FOUND` | Build-reviewed; hardware pending |
| Save valid open network | State `VALID`; load `ESP_OK` | Hardware pending |
| Save valid secured network | State `VALID`; load `ESP_OK` | Hardware pending |
| Clear after save | State `NOT_CONFIGURED`; `has=false` | Hardware pending |
| SSID key only | State `INCOMPLETE` | Hardware corruption test pending |
| Password key only | State `INCOMPLETE` | Hardware corruption test pending |
| Missing version with credentials | State `INCOMPLETE` | Hardware corruption test pending |
| Unsupported version | State `UNSUPPORTED_VERSION` | Hardware corruption test pending |
| Wrong key type | State `INVALID_DATA` | Hardware corruption test pending |
| Oversized stored string | State `INVALID_DATA` | Hardware corruption test pending |
| Concurrent operations | Serialized or `ESP_ERR_TIMEOUT` | Stress test pending |
| Full project compile/link | No new compiler error | Completed |

## Explicit Non-Goals

Phase 5.3B does not add:

- Schema migration.
- Device identity storage.
- Factory reset.
- Boot integration or removal of hard-coded credentials.
- BLE/SoftAP provisioning.
- Wi-Fi connection, retry, GUI, cloud, or reboot behavior.
