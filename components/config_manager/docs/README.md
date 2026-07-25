# config_manager

## Purpose

`config_manager` is the storage-only owner of persistent application
configuration in ESP-IDF NVS. It provides:

- Versioned Wi-Fi credential persistence and integrity classification.
- Explicit migration from supported legacy Wi-Fi schema v0 to v1.
- Fail-closed protection for interrupted multi-key Wi-Fi updates.
- Optional device identity persistence.
- Typed caller-defined custom data in a separate namespace.
- Wi-Fi-only clear, identity-only clear, and component-scoped factory reset.
- Per-operation NVS handles and FreeRTOS mutex serialization.
- Bounded reads, cleared failure outputs, and credential zeroization.

This README is the single design, usage, test, and acceptance document for the
component.

## Ownership Boundary

The component owns:

- Validation of public configuration types.
- The `device_cfg` and `custom_cfg` namespaces.
- Schema detection and explicit supported migration.
- NVS open, read/write/erase, commit, and close lifecycle.
- Serialization of each complete public storage operation.
- Clearing temporary credential snapshots.

The component does not:

- Call `nvs_flash_init()`, `nvs_flash_erase()`, or `esp_restart()`.
- Generate device identity.
- Connect, disconnect, or retry Wi-Fi.
- Call GUI, cloud, Firebase, provisioning, callback, or reboot APIs.
- Own application boot policy.
- Implement BLE/SoftAP provisioning or reset-button handling.
- Configure NVS encryption.

The application owns NVS initialization, identity policy, boot decisions, and
conversion from copied config-manager types to subsystem-specific types.

## Component Structure

```text
components/config_manager/
|-- CMakeLists.txt
|-- config_manager.c
|-- include/
|   `-- config_manager.h
`-- docs/
    `-- README.md
```

The destructive component test application is independent of the production
component:

```text
Test/config_manager/
|-- CMakeLists.txt
|-- README.md
|-- .gitignore
|-- sdkconfig.defaults
`-- main/
    |-- CMakeLists.txt
    `-- test_config_manager.c
```

No production fault-injection API is exposed.

## Initialization

The application must initialize default NVS before this component:

```c
esp_err_t err = nvs_flash_init();

if (err == ESP_OK)
{
    err = config_manager_init();
}
```

`config_manager_init()` creates only internal runtime state and a non-recursive
mutex. Repeated calls after successful initialization return `ESP_OK`. Storage
APIs must not be called from an ISR.

Production code does not automatically erase the default NVS partition when
initialization reports corruption or an incompatible on-flash format.

## Data Model

| Namespace | Key | NVS type | Purpose |
| --- | --- | --- | --- |
| `device_cfg` | `cfg_ver` | `u32` | Wi-Fi schema/version state |
| `device_cfg` | `wifi_ssid` | string | Wi-Fi SSID |
| `device_cfg` | `wifi_pass` | string | Wi-Fi password |
| `device_cfg` | `device_id` | string | Optional device identifier |
| `device_cfg` | `device_name` | string | Optional display name |
| `custom_cfg` | caller-defined | typed | Optional caller-owned data |

`device_cfg/cfg_ver` governs only the Wi-Fi credential schema. It does not
govern `custom_cfg`, and optional identity does not force a schema increment.
Custom APIs cannot write arbitrary keys into `device_cfg`.

## Public Types

### `config_manager_wifi_config_t`

- `ssid`: 1-32 bytes plus null terminator.
- `password`: empty for an open network, or 8-63 bytes plus null terminator.

### `config_manager_device_identity_t`

- `device_id`: 1-36 bytes plus null terminator.
- `device_name`: 1-32 bytes plus null terminator.

The component validates and stores identity but does not generate it.

### `config_manager_data_type_t`

Supported custom representations:

- Signed and unsigned 8, 16, 32, and 64-bit integers.
- Null-terminated strings.
- Blobs containing 1-512 bytes.

Custom keys contain 1-15 bytes. Integer sizes must exactly match the selected
type. String/blob reads support a size query with `out_value == NULL`.

## Public APIs

### Initialization

- `config_manager_init()`

### Wi-Fi and schema

- `config_manager_save_wifi()`
- `config_manager_load_wifi()`
- `config_manager_clear_wifi()`
- `config_manager_get_wifi_config_state()`
- `config_manager_has_wifi_config()`
- `config_manager_migrate_device_config()`

### Device identity

- `config_manager_save_device_identity()`
- `config_manager_load_device_identity()`
- `config_manager_clear_device_identity()`
- `config_manager_has_device_identity()`

### Custom data and reset

- `config_manager_save_custom_data()`
- `config_manager_load_custom_data()`
- `config_manager_clear_custom_data()`
- `config_manager_factory_reset()`

The public header contains the authoritative Doxygen contract and error
semantics for each API.

## NVS Lifecycle

Every public storage operation follows:

```text
validate
-> lock
-> open NVS
-> read/write/erase
-> commit only for a mutation
-> close NVS
-> unlock
```

Rules:

- No global NVS handle is retained.
- Every successful lock has one unlock.
- Every successful `nvs_open()` has one `nvs_close()`.
- Handles are closed before the mutex is released.
- Read operations do not commit.
- Write and erase operations commit only mutation paths.
- No external subsystem API is called while the mutex is held.
- Lock acquisition times out after 1000 ms with `ESP_ERR_TIMEOUT`.
- Public APIs return `esp_err_t`; reusable component code does not use
  `ESP_ERROR_CHECK()`.

## Wi-Fi State Model

| State | Meaning |
| --- | --- |
| `UNKNOWN` | Inspection failed before semantic classification completed |
| `NOT_CONFIGURED` | Neither credential key exists |
| `VALID` | Complete valid credentials use current schema v1 |
| `INCOMPLETE` | Exactly one credential key exists |
| `UNSUPPORTED_VERSION` | Complete credentials use an unsupported version |
| `INVALID_DATA` | A key has the wrong type, length, or semantic value |
| `MIGRATION_REQUIRED` | Complete valid legacy v0 credentials require migration |

`config_manager_get_wifi_config_state()` returns semantic states with `ESP_OK`.
Storage or synchronization failures return an error and leave the state as
`UNKNOWN`.

ESP-IDF typed getters may report a wrong-type key as not found. The component
uses `nvs_find_key()` before typed reads so missing and malformed keys remain
distinct.

## Schema and Migration

| Version | Representation | Policy |
| --- | --- | --- |
| v0 | Valid credentials with missing `cfg_ver` or `cfg_ver = 0` | Supported legacy |
| v1 | Valid credentials with `cfg_ver = 1` | Current |
| `UINT32_MAX` | Wi-Fi update was interrupted or did not complete | Fail closed |
| Other | Complete credentials with another version | Unsupported, preserved |

Read APIs never migrate implicitly. Migration is forward-only and idempotent.
Unsupported data and invalid legacy data are never erased or overwritten.

Migration flow:

```c
config_manager_wifi_config_state_t state =
    CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

esp_err_t err =
    config_manager_get_wifi_config_state(&state);

if (err == ESP_OK &&
    state == CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED)
{
    err = config_manager_migrate_device_config();
}
```

Migration validates legacy credentials, writes only current `cfg_ver`, commits
once, and preserves SSID/password bytes. Calling it again for v1 is an
`ESP_OK` no-op.

## Fail-Closed Wi-Fi Save

Wi-Fi configuration uses separate NVS keys, so a single atomic transaction
cannot cover the whole credential pair. `config_manager_save_wifi()` uses a
three-stage publication sequence:

```text
persist cfg_ver = UINT32_MAX
-> persist SSID and password while marker remains active
-> publish cfg_ver = 1
```

If reset, power loss, or flash failure interrupts either of the first two
stages, boot sees a non-valid state such as `NOT_CONFIGURED`, `INCOMPLETE`, or
`UNSUPPORTED_VERSION` and does not connect. The caller can retry
`config_manager_save_wifi()` to replace the interrupted update.

This approach favors refusing a potentially mixed credential pair over
silently attempting a connection.

Saving credentials that already match a valid current configuration returns
`ESP_OK` without another write or commit.

## Device Identity

Both identity keys must be present and valid:

- Both absent: `ESP_ERR_NVS_NOT_FOUND`.
- Exactly one present: `ESP_ERR_INVALID_STATE`.
- Wrong type, excessive length, or invalid value:
  `ESP_ERR_INVALID_RESPONSE`.
- Both complete and valid: `ESP_OK`.

Load output is cleared before NVS access and remains cleared on failure.

## Custom Data

Custom data is stored only in `custom_cfg`.

- Fixed integers require exact caller buffer sizes.
- String/blob reads can query required size.
- A supplied output buffer is cleared before NVS access.
- Wrong stored type returns `ESP_ERR_NVS_TYPE_MISMATCH`.
- Blob contents and custom values are never logged.

Do not use this API for administrator secrets, private keys, or long-lived
cloud credentials.

## Clear and Reset Semantics

| Operation | Credentials | Schema | Identity | Custom data |
| --- | --- | --- | --- | --- |
| `config_manager_clear_wifi()` | Erased | Preserved | Preserved | Preserved |
| `config_manager_clear_device_identity()` | Preserved | Preserved | Erased | Preserved |
| `config_manager_factory_reset()` | Erased | Erased | Erased | Erased |

Factory reset touches only `device_cfg` and `custom_cfg`. It does not erase the
complete default NVS partition, disconnect Wi-Fi, or reboot.

NVS has no transaction across namespaces. Factory reset therefore attempts
both namespaces independently and returns the first error. A flash failure can
still leave one namespace reset and the other unchanged.

## Production Boot Integration

Production boot performs:

```text
nvs_flash_init
-> config_manager_init
-> inspect Wi-Fi state
-> migrate once when required
-> inspect again
-> initialize network platform
-> initialize wifi_manager
-> load and connect only for VALID
```

No config-manager mutex is held when `wifi_manager_connect()` runs. Missing,
incomplete, invalid, interrupted, and unsupported data are preserved and do
not start a connection.

## Buffer and Security Rules

- Wi-Fi and identity save operations validate stable local snapshots.
- Wi-Fi and identity load outputs are cleared before access and on failure.
- Supplied custom-data load buffers are cleared before NVS access.
- The application passes `wifi_manager_connect()` a copied credential object
  after config-manager closes NVS and releases its mutex.
- Temporary Wi-Fi copies are explicitly zeroized.
- Wi-Fi passwords are never logged.
- Blob and custom-data contents are never logged.
- Firmware source must not contain production Wi-Fi credentials.
- NVS encryption is not configured by this component.

## Usage Examples

### Save and load Wi-Fi

```c
config_manager_wifi_config_t saved = {
    .ssid = "demo-network",
    .password = "demo-pass-123",
};

esp_err_t err = config_manager_save_wifi(&saved);

config_manager_wifi_config_t loaded = {0};

if (err == ESP_OK)
{
    err = config_manager_load_wifi(&loaded);
}
```

### Save and load identity

```c
config_manager_device_identity_t saved = {
    .device_id = "demo-device-001",
    .device_name = "Smart Room Demo",
};

config_manager_device_identity_t loaded = {0};

esp_err_t err =
    config_manager_save_device_identity(&saved);

if (err == ESP_OK)
{
    err = config_manager_load_device_identity(&loaded);
}
```

### Save custom data

```c
uint32_t interval_seconds = 10U;

esp_err_t err = config_manager_save_custom_data(
    "upload_sec",
    &interval_seconds,
    sizeof(interval_seconds),
    CONFIG_MANAGER_DATA_TYPE_U32);
```

### Clear and factory reset

```c
esp_err_t err = config_manager_clear_wifi();

/* Storage-only reset; does not disconnect or reboot. */
err = config_manager_factory_reset();
```

## Test Application

The isolated destructive test firmware is located at:

```text
Test/config_manager/
```

It tests initialization, validation, missing and corrupt NVS keys, schema
migration, interrupted Wi-Fi publication, device identity, custom data,
clear/reset scope, repeated operations, and concurrency.

```powershell
cd Test/config_manager
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

The test image erases default NVS at boot. Do not flash it to a device whose
stored configuration must be preserved.

Expected current Unity summary after the added interrupted-write test:

```text
38 Tests 0 Failures 0 Ignored
```

## Verification Status

Completed before this cleanup:

- Production ESP32-S3 compile/link with ESP-IDF v6.0.1.
- Original 14-test Phase 5.3B hardware run.
- Expanded 37-test Sprint 5 hardware run:
  `37 Tests 0 Failures 0 Ignored`.
- Production `NOT_CONFIGURED` boot behavior.

Completed by this cleanup:

- Production firmware compile/link with ESP-IDF v6.0.1.
- Renamed 38-case `Test/config_manager` firmware compile/link.

Still required:

- Run the updated 38-test suite on ESP32-S3 hardware.
- Verify Wi-Fi configuration persists across reboot.
- Verify device identity persists across reboot.
- Exercise production boot with `VALID`, `MIGRATION_REQUIRED`, `INCOMPLETE`,
  `INVALID_DATA`, and `UNSUPPORTED_VERSION`.

The destructive test app erases default NVS at boot, so it cannot by itself
prove reboot persistence.

## Known Limitations

- Factory reset cannot be atomic across two NVS namespaces.
- No schema beyond v1 exists.
- No device identity generation policy exists.
- Persisted identity is not yet applied to the fixed cloud endpoint.
- No NVS encryption setup.
- No BLE/SoftAP provisioning.
- No physical reset-button handling.

## Sprint Boundary

Sprint 5 implementation is complete, but the updated hardware acceptance
evidence remains pending. BLE/SoftAP provisioning belongs to Sprint 6 and must
not be implemented from this component.
