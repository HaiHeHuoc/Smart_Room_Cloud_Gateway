# config_manager Component Notes

## Purpose

`config_manager` owns persistent application configuration stored in ESP-IDF
NVS. The current implementation stores versioned Wi-Fi credentials and provides
a separate namespace for small typed custom values.

The component owns validation, NVS handle lifetime, commit behavior, and
serialization of its public operations. The application must initialize the
default NVS flash partition before calling `config_manager_init()`.

## Responsibility

- Validate and save Wi-Fi SSID/password data.
- Load only complete Wi-Fi data using the supported format version.
- Check whether valid Wi-Fi configuration exists.
- Clear Wi-Fi credentials without touching custom data.
- Save, load, and clear supported custom NVS values.
- Open and close an NVS handle inside every storage operation.
- Serialize complete logical operations with an internal mutex.
- Keep passwords out of logs and wipe temporary credential snapshots.

## Non-Responsibility

`config_manager` does not:

- Initialize or erase the ESP-IDF NVS flash partition.
- Connect, disconnect, or retry Wi-Fi.
- Call Firebase, cloud, GUI, or provisioning APIs.
- Generate device identity.
- Reboot the device.
- Own application policy for missing or corrupt configuration.

The application layer converts `config_manager_wifi_config_t` to the input type
expected by `wifi_manager`. `wifi_manager` must copy credentials needed by an
asynchronous connection attempt.

## Initialization

Required order:

```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(config_manager_init());
```

`config_manager_init()` creates the operation mutex. It is idempotent after a
successful call. A concurrent call while the first initialization is still in
progress returns `ESP_ERR_INVALID_STATE` instead of creating and leaking a
second mutex.

## NVS Layout

### Wi-Fi namespace

Namespace: `device_cfg`

| Key | NVS type | Meaning |
| --- | --- | --- |
| `cfg_ver` | `u32` | Current stored schema version (`1`). |
| `wifi_ssid` | string | Null-terminated Wi-Fi SSID. |
| `wifi_pass` | string | Empty for an open network, otherwise the password. |

Wi-Fi save stages all three values and calls `nvs_commit()` once. Loading
rejects any version other than the current version with
`ESP_ERR_NOT_SUPPORTED`. Missing or incomplete data returns the underlying NVS
error, normally `ESP_ERR_NVS_NOT_FOUND`.

`config_manager_clear_wifi()` erases only `wifi_ssid` and `wifi_pass`. It keeps
`cfg_ver` because the namespace may later contain device identity using the
same schema version.

### Custom namespace

Namespace: `custom_cfg`

Custom keys contain 1-15 bytes and support:

- `uint8_t`, `int8_t`
- `uint16_t`, `int16_t`
- `uint32_t`, `int32_t`
- `uint64_t`, `int64_t`
- Null-terminated strings
- Blobs from 1 to 512 bytes when saved through this component

The custom namespace is independent from Wi-Fi clear operations.

## Public API

| API | Current role |
| --- | --- |
| `config_manager_init()` | Initialize mutex and component state. |
| `config_manager_save_wifi()` | Validate, version, and commit Wi-Fi credentials. |
| `config_manager_load_wifi()` | Load current-version Wi-Fi credentials into a cleared output. |
| `config_manager_has_wifi_config()` | Report whether a complete valid Wi-Fi configuration loads successfully. |
| `config_manager_clear_wifi()` | Idempotently erase only Wi-Fi credential keys. |
| `config_manager_save_custom_data()` | Commit one typed value in `custom_cfg`. |
| `config_manager_load_custom_data()` | Load one typed value or query string/blob size. |
| `config_manager_clear_custom_data()` | Idempotently erase one custom key. |

## Wi-Fi Validation

- SSID must be null-terminated and contain 1-32 bytes.
- An empty password represents an open network.
- A non-empty password must be null-terminated and contain 8-63 bytes.
- Partial, oversized, unterminated, or unsupported-version data is not exposed
  to the application as valid configuration.
- `config_manager_load_wifi()` clears the caller output before NVS access and
  clears it again on every failure.

## Custom Data Contract

For fixed-size integer types, `value_size` or `*inout_size` must exactly match
the selected C integer type. Integer reads use local aligned values and
`memcpy()`, avoiding unaligned access through a caller-provided `void *`.

For strings and blobs, `config_manager_load_custom_data()` supports the normal
NVS size-query pattern:

```c
size_t required_size = 0U;

esp_err_t error = config_manager_load_custom_data(
    "example",
    NULL,
    &required_size,
    CONFIG_MANAGER_DATA_TYPE_STRING);
```

The caller then allocates or supplies a sufficiently sized buffer and calls the
API again. Stored NVS types must match the requested
`config_manager_data_type_t`.

## NVS Handle Lifecycle

Every operation follows this pattern:

```text
validate arguments
    -> acquire component mutex
    -> nvs_open()
    -> get/set/erase
    -> nvs_commit() for changes
    -> nvs_close() on every opened-handle path
    -> release mutex
```

The component does not retain a global NVS handle. Erase operations commit only
when a key actually existed; clearing an already missing key still returns
`ESP_OK`.

## Thread Safety

- Public storage operations are serialized by one internal FreeRTOS mutex.
- The 1000 ms mutex timeout returns `ESP_ERR_TIMEOUT` rather than blocking
  forever.
- The mutex protects the complete multi-step NVS transaction, not one NVS call
  at a time.
- No Wi-Fi, cloud, GUI, callback, or reboot API is called while holding it.
- APIs are task-context APIs and must not be called from an ISR.
- There is currently no deinit path, so the mutex remains valid for firmware
  lifetime after successful initialization.

## Security Notes

- Password contents are never logged.
- Temporary Wi-Fi snapshots are explicitly zeroized before their stack storage
  is released, including successful save/load/check paths.
- NVS encryption is not enabled or managed by this component. Plain NVS storage
  does not protect credentials against physical flash access.
- Do not use custom-data APIs to store Firebase administrator credentials,
  service-account private keys, ID tokens, or refresh tokens.

## Error Behavior

Important return values include:

- `ESP_ERR_INVALID_STATE`: API called before initialization, or concurrent init.
- `ESP_ERR_TIMEOUT`: operation mutex was not acquired within 1000 ms.
- `ESP_ERR_INVALID_ARG`: invalid pointer, key, type, or malformed Wi-Fi data.
- `ESP_ERR_INVALID_SIZE`: incorrect typed-data size or oversized blob.
- `ESP_ERR_NVS_NOT_FOUND`: missing namespace/key or incomplete Wi-Fi data.
- `ESP_ERR_NVS_TYPE_MISMATCH`: stored custom type differs from requested type.
- `ESP_ERR_NOT_SUPPORTED`: unsupported Wi-Fi configuration version.
- Other NVS errors are returned without collapsing them to `ESP_FAIL`.

## Known Limitations

- Device ID and device name persistence are not implemented yet.
- There is no component-level factory-reset API yet.
- Version migration is not implemented; only version `1` is accepted.
- Wi-Fi boot integration and removal of hard-coded credentials are application
  work and are not implemented by this component.
- There is no deinit API.
- NVS encryption policy remains an application/product decision.
- Hardware reboot-persistence, corrupt-data, repeated-operation, and resource
  leak acceptance tests are still required before Phase 5 completion.
