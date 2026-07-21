# Phase 5.3B - Wi-Fi Configuration Integrity and State Handling

## Status

The Phase 5.3B implementation and fault-injection harness are complete. The
first ESP32-S3 run executed all 14 Unity tests: 13 passed and the wrong-type
SSID test exposed an integrity-classification defect. The defect is fixed and
both firmware targets build with ESP-IDF v6.0.1, but a hardware rerun is still
required before this phase is fully verified.

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

In ESP-IDF v6.0.1, a typed getter such as `nvs_get_str()` can return
`ESP_ERR_NVS_NOT_FOUND` when a key exists under a different NVS type. The
inspection path first calls `nvs_find_key()` for each required key and verifies
the actual type before using the typed getter. This distinguishes a genuinely
missing key from wrong-type stored data.

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
- A standalone Unity test app now injects malformed NVS states without adding
  a production fault-injection API.
- Required key types are inspected explicitly so a wrong-type SSID, password,
  or version cannot be mistaken for a missing key.

## Fault-Injection Test App

Location:

```text
Test/config_manager_phase_5_3b/
```

The app uses the production `config_manager` component through
`EXTRA_COMPONENT_DIRS`. Its raw NVS helpers are private to the test binary and
run sequentially while no component operation is active. The production
header and component CMake file contain no test hook.

The test app performs these steps:

1. Erase and initialize the default NVS partition.
2. Verify operation errors before `config_manager_init()`.
3. Initialize the component once.
4. Run public-API and raw-NVS fault-injection tests with Unity.
5. Clear the three Wi-Fi keys in `setUp()`, `tearDown()`, and final cleanup.
6. Leave the Wi-Fi state as `NOT_CONFIGURED`.

The firmware is intentionally destructive because it erases default NVS on
every boot. Use a test device or back up any configuration that must be kept.

Build and run:

```powershell
cd Test/config_manager_phase_5_3b
idf.py -B build -DIDF_TARGET=esp32s3 build
idf.py -B build -p <PORT> flash monitor
```

Tests execute automatically. Each test logs expected and actual state/result,
then Unity reports PASS or FAIL. Password contents are not logged.

Expected final evidence for a successful board run:

```text
14 Tests 0 Failures 0 Ignored
Final cleanup: clear=ESP_OK, state_result=ESP_OK, state=NOT_CONFIGURED
Phase 5.3B test run complete: failures=0
```

## Verification Performed

Completed in the current development environment:

- Reviewed state classification and the shared inspection helper.
- Reviewed output clearing, credential snapshot zeroization, handle lifecycle,
  mutex symmetry, commit rules, and secret-free logging.
- Compiled and linked the production ESP32-S3 firmware.
- Compiled and linked the standalone ESP32-S3 Unity test firmware.
- Confirmed no test code is included by the production project CMake graph.
- Reviewed the first hardware run supplied by the tester: 14 tests executed,
  13 passed, and wrong-type SSID classification failed before the fix.

Not executed in the current environment:

- Hardware rerun of the corrected wrong-type classification.
- Reboot-persistence tests.
- Concurrent-operation or mutex-timeout stress tests.

## Acceptance Matrix

| Scenario | Expected result | Verification status |
| --- | --- | --- |
| Operation before init | Error; state `UNKNOWN`; load output empty | Passed on first hardware run |
| Missing namespace | State `NOT_CONFIGURED`; load `ESP_ERR_NVS_NOT_FOUND` | Passed on first hardware run |
| Save valid open network | State `VALID`; load `ESP_OK` | Passed on first hardware run |
| Save valid secured network | State `VALID`; load `ESP_OK` | Passed on first hardware run |
| Clear after save, version retained | State `NOT_CONFIGURED`; load output empty | Passed on first hardware run |
| Version and SSID only | State `INCOMPLETE` | Passed on first hardware run |
| Password only | State `INCOMPLETE` | Passed on first hardware run |
| SSID and password without version | State `INCOMPLETE` | Passed on first hardware run |
| Unsupported version | State remains `UNSUPPORTED_VERSION`; data not erased | Passed on first hardware run |
| SSID stored as `u32` | State `INVALID_DATA`; load output empty | Failed before fix; corrected build pending rerun |
| Oversized stored SSID | State `INVALID_DATA`; load output empty | Passed on first hardware run |
| Empty stored SSID | State `INVALID_DATA`; load output empty | Passed on first hardware run |
| Stored password length 1-7 | State `INVALID_DATA`; load output empty | Passed on first hardware run |
| Production firmware compile/link | No compiler or linker error | Completed |
| Test firmware compile/link | No compiler or linker error | Completed |
| Concurrent operations | Serialized or `ESP_ERR_TIMEOUT` | Not implemented; deferred stress test |

## Explicit Non-Goals

Phase 5.3B does not add:

- Schema migration.
- Device identity storage.
- Factory reset.
- Boot integration or removal of hard-coded credentials.
- BLE/SoftAP provisioning.
- Wi-Fi connection, retry, GUI, cloud, or reboot behavior.
