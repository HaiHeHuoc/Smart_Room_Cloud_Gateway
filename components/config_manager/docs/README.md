# config_manager

## Purpose

`config_manager` owns persistent application configuration stored in ESP-IDF
NVS. The implementation completed through Phase 5.3B provides:

- Versioned Wi-Fi credential persistence and integrity classification.
- Independent typed custom-data storage.
- Per-operation NVS handle ownership.
- Internal serialization with a FreeRTOS mutex.
- Secret-free logging and cleanup of temporary credential snapshots.

Detailed Phase 5.3B behavior is documented in
[`phase_5_3b_config_integrity.md`](phase_5_3b_config_integrity.md).

## Ownership Boundary

The component owns:

- Validation of its public input types.
- The `device_cfg` and `custom_cfg` namespaces.
- NVS open, read/write, commit, and close lifecycle for each operation.
- Classification of missing, incomplete, unsupported, and invalid Wi-Fi data.
- Serialization of complete public storage operations.

The component does not:

- Call `nvs_flash_init()` or erase the default NVS partition.
- Connect, disconnect, or retry Wi-Fi.
- Call GUI, cloud, Firebase, provisioning, or reboot APIs.
- Generate device identity or implement factory-reset policy.
- Enable or manage NVS encryption.

The application owns NVS flash initialization and converts copied credentials
to the type required by `wifi_manager`.

## Initialization

Call the APIs in this order from the application layer:

```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(config_manager_init());
```

`config_manager_init()` creates only internal runtime state and the operation
mutex. It is idempotent after successful initialization. A concurrent call
while initialization is still in progress returns `ESP_ERR_INVALID_STATE`.

Do not call storage APIs from an ISR.

## NVS Layout

### Wi-Fi configuration

Namespace: `device_cfg`

| Key | NVS type | Meaning |
| --- | --- | --- |
| `cfg_ver` | `u32` | Stored schema version. Current value is `1`. |
| `wifi_ssid` | string | Null-terminated SSID containing 1-32 bytes. |
| `wifi_pass` | string | Empty for an open network, otherwise 8-63 bytes. |

`config_manager_save_wifi()` stages all three keys and calls `nvs_commit()`
once. `config_manager_clear_wifi()` erases only `wifi_ssid` and `wifi_pass` and
preserves `cfg_ver`. With no credential keys present, the Wi-Fi state is
`CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED` even when `cfg_ver` remains.

### Custom data

Namespace: `custom_cfg`

Custom keys contain 1-15 bytes. Supported value types are:

- `uint8_t`, `int8_t`
- `uint16_t`, `int16_t`
- `uint32_t`, `int32_t`
- `uint64_t`, `int64_t`
- Null-terminated strings
- Blobs containing 1-512 bytes when saved through this component

Custom APIs never read or write the `device_cfg` namespace.

## Public API

| API | Current behavior |
| --- | --- |
| `config_manager_init()` | Initialize internal state and mutex. |
| `config_manager_save_wifi()` | Validate and persist version, SSID, and password. |
| `config_manager_load_wifi()` | Return only complete, supported, valid credentials. |
| `config_manager_clear_wifi()` | Idempotently erase SSID and password. |
| `config_manager_get_wifi_config_state()` | Inspect integrity without returning credentials. |
| `config_manager_has_wifi_config()` | Report whether a valid configuration can be loaded. |
| `config_manager_save_custom_data()` | Save and commit one typed custom value. |
| `config_manager_load_custom_data()` | Load one typed value or query string/blob size. |
| `config_manager_clear_custom_data()` | Idempotently erase one custom key. |

## Wi-Fi State Model

| State | Meaning |
| --- | --- |
| `UNKNOWN` | Inspection did not complete because of an API, mutex, or NVS error. |
| `NOT_CONFIGURED` | Neither SSID nor password is stored. `cfg_ver` may remain. |
| `VALID` | All keys exist, version `1` is supported, and values pass validation. |
| `INCOMPLETE` | At least one credential exists, but another required key is missing. |
| `UNSUPPORTED_VERSION` | All required keys exist but `cfg_ver` is not `1`. |
| `INVALID_DATA` | A key has the wrong type, excessive length, or invalid content. |

`config_manager_get_wifi_config_state()` returns `ESP_OK` for every completed
semantic classification. It returns a non-OK error only when inspection itself
cannot complete, and then leaves the state as `UNKNOWN`.

ESP-IDF typed getters can report a wrong-type key as `ESP_ERR_NVS_NOT_FOUND`.
The integrity inspection therefore uses `nvs_find_key()` to verify each
required key's stored type before reading its value. This keeps a malformed key
in `INVALID_DATA` instead of misclassifying it as a missing key.

`config_manager_load_wifi()` maps semantic states to load results:

| State | Load result |
| --- | --- |
| `NOT_CONFIGURED` | `ESP_ERR_NVS_NOT_FOUND` |
| `VALID` | `ESP_OK` |
| `INCOMPLETE` | `ESP_ERR_INVALID_STATE` |
| `UNSUPPORTED_VERSION` | `ESP_ERR_NOT_SUPPORTED` |
| `INVALID_DATA` | `ESP_ERR_INVALID_RESPONSE` |

The load destination is cleared before NVS access and remains cleared on every
failure.

## Custom Data Contract

Fixed-size integer operations require an exact size match. Reads use aligned
local values and copy to the caller only after `nvs_get_*()` succeeds.

String and blob reads support the NVS size-query pattern:

```c
size_t required_size = 0U;

esp_err_t err = config_manager_load_custom_data(
    "example",
    NULL,
    &required_size,
    CONFIG_MANAGER_DATA_TYPE_STRING);
```

The caller can then provide a buffer of `required_size` bytes and call the API
again. A stored type mismatch is returned as `ESP_ERR_NVS_TYPE_MISMATCH`.

## NVS Lifecycle

Each public storage operation follows this sequence:

```text
validate
    -> lock
    -> nvs_open()
    -> read, set, or erase
    -> nvs_commit() only for a successful mutation
    -> nvs_close()
    -> unlock
```

The component does not retain a global NVS handle. Every successfully opened
handle is closed before the mutex is released. Read operations do not commit.
Idempotent erase operations skip commit when no key changed.

## Thread Safety

- One internal mutex serializes each complete storage operation.
- Lock acquisition times out after 1000 ms with `ESP_ERR_TIMEOUT`.
- The mutex is never held across Wi-Fi, GUI, cloud, callback, provisioning, or
  reboot calls.
- There is no deinit API; the mutex remains valid for firmware lifetime after
  initialization.

## Security

- Wi-Fi passwords are never logged.
- Temporary Wi-Fi snapshots are explicitly zeroized before returning.
- Custom values and blob contents are never logged.
- NVS encryption is outside this component and is not currently guaranteed.
- Do not store administrator secrets, private keys, or long-lived cloud tokens
  through the custom-data API.

## Phase 5.3B Test Harness

The destructive hardware test app is isolated from the production build at:

```text
Test/config_manager_phase_5_3b/
```

It links the production `config_manager` component without adding fault-
injection APIs to the public header. Raw NVS writes exist only in the test
binary and cover valid, missing, incomplete, wrong-type, oversized,
unsupported-version, and semantically invalid configurations.

The test image erases the default NVS partition when it boots. Do not flash it
to a device whose stored configuration must be preserved.

Build and run it from the test-app directory:

```powershell
idf.py -B build -DIDF_TARGET=esp32s3 build
idf.py -B build -p <PORT> flash monitor
```

The tests run automatically after boot. A successful hardware run ends with a
Unity summary containing `14 Tests 0 Failures` and a final cleanup state of
`NOT_CONFIGURED`. Password contents are never printed; valid-load checks report
only `password_match=YES` or `password_match=NO`.

Current verification status:

- Production source review: completed.
- Production firmware compile and link: completed with ESP-IDF v6.0.1.
- Test firmware compile and link: completed with ESP-IDF v6.0.1.
- First ESP32-S3 run: 14 tests executed, 13 passed, and the wrong-type SSID
  case exposed an `INCOMPLETE` versus `INVALID_DATA` classification defect.
- Wrong-type classification fix: implemented and build-verified; hardware
  confirmation rerun is pending.
- Reboot-persistence and concurrent stress execution: pending.

## Important Errors

- `ESP_ERR_INVALID_ARG`: invalid pointer, key, type, or malformed input.
- `ESP_ERR_INVALID_SIZE`: incorrect typed size or unsupported blob size.
- `ESP_ERR_INVALID_STATE`: use before initialization or incomplete Wi-Fi data.
- `ESP_ERR_TIMEOUT`: mutex acquisition timeout.
- `ESP_ERR_NVS_NOT_FOUND`: requested data is not configured or stored.
- `ESP_ERR_NVS_TYPE_MISMATCH`: custom value has a different stored type.
- `ESP_ERR_NOT_SUPPORTED`: Wi-Fi schema version is unsupported.
- `ESP_ERR_INVALID_RESPONSE`: stored Wi-Fi data is corrupt or invalid.
- Other NVS errors are returned to the caller without conversion to
  `ESP_FAIL`.

## Current Limitations

The following work is intentionally outside Phase 5.3B:

- Version migration.
- Device ID and device name persistence.
- Component-level factory reset.
- Boot-time replacement of hard-coded Wi-Fi credentials.
- BLE or SoftAP provisioning.
- Wi-Fi reconnect policy and application state transitions.
- NVS encryption policy.

The Phase 5.3B fault-injection harness has completed one ESP32-S3 run. That run
exposed one wrong-type classification defect, which is fixed and build-
verified. A hardware rerun is still required before recording `14 Tests 0
Failures`. Reboot persistence, concurrent stress, and long-run resource
stability remain acceptance work.
