# wifi_manager Component Notes

## Purpose

`wifi_manager` owns the ESP32-S3 Wi-Fi Station driver and its connection
status. It creates the default Station network interface, handles Wi-Fi/IP
events, starts connections with application-provided credentials, exposes
status snapshots and RSSI, and can report nearby access points to the serial
log.

This is the Phase 2 implementation. Credentials are supplied by the
application and stored by the Wi-Fi driver in RAM. NVS credential ownership,
BLE provisioning, automatic reconnect, and connection backoff are not part of
this component yet.

## What Is Implemented

- Creates the default ESP-NETIF Wi-Fi Station interface.
- Initializes and starts the ESP-IDF Wi-Fi driver in Station mode.
- Uses `WIFI_STORAGE_RAM`, so credentials are not persisted by this component.
- Registers handlers for all `WIFI_EVENT` and `IP_EVENT` events.
- Tracks SSID, state, IPv4 address, RSSI validity, and disconnect reason.
- Copies status under a FreeRTOS critical-section lock.
- Supports one application status callback with optional user data.
- Reads connected AP RSSI using `esp_wifi_sta_get_rssi()`.
- Performs a blocking active scan and logs SSID, channel, RSSI, and quality.
- Cleans up partially initialized Wi-Fi resources when initialization fails.

## State Model

The current state progression is:

```text
UNINITIALIZED
    -> READY
    -> CONNECTING
    -> WAITING_FOR_IP
    -> CONNECTED
```

Other transitions:

```text
CONNECTING -> FAILED       when esp_wifi_connect() cannot start
any active state -> DISCONNECTED on WIFI_EVENT_STA_DISCONNECTED
```

`CONNECTED` means the Station has received an IPv4 address, not only that it
has associated with an access point.

## Initialization Order

The application must initialize shared ESP-IDF network infrastructure before
calling `wifi_manager_init()`:

```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(wifi_manager_init());
```

In this project those first three operations are currently owned by
`network_platform_init()` in `main.c`.

## Basic Usage

Register the status callback before starting the connection if the application
must observe the initial `CONNECTING` state:

```c
ESP_ERROR_CHECK(
    wifi_manager_register_status_callback(
        app_wifi_status_callback,
        NULL
    )
);

const wifi_manager_sta_config_t config = {
    .ssid = "YOUR_SSID",
    .password = "YOUR_PASSWORD",
};

ESP_ERROR_CHECK(wifi_manager_connect(&config));
```

The config structure and its strings only need to remain valid for the duration
of `wifi_manager_connect()`. The function copies them into ESP-IDF's Station
configuration.

## Public API

| API | Current behavior |
| --- | --- |
| `wifi_manager_init()` | Initialize and start the Station manager once. |
| `wifi_manager_connect()` | Validate/copy credentials and begin asynchronous connection. |
| `wifi_manager_disconnect()` | Currently returns `ESP_ERR_NOT_SUPPORTED`. |
| `wifi_manager_get_status()` | Copy a locked snapshot of current manager status. |
| `wifi_manager_get_rssi()` | Read and cache RSSI while connected with IPv4. |
| `wifi_manager_is_connected()` | Report whether state is `CONNECTED` and IPv4 is valid. |
| `wifi_manager_register_status_callback()` | Register, replace, or unregister the single callback. |
| `wifi_manager_state_to_string()` | Convert a state enum to constant readable text. |
| `wifi_manager_scan_and_log()` | Block while scanning and print AP results. |

## Status Callback And LVGL

The callback may run from the ESP event-loop task. It must return quickly and
must not call LVGL directly.

The project uses this flow:

```text
ESP Wi-Fi/IP event
    -> wifi_manager updates status
    -> application status callback
    -> app_gui_post_wifi_status()
    -> FreeRTOS queue
    -> app_gui task receives and processes status under LVGL coordination
```

The callback receives a temporary status snapshot. Copy any required values
before returning; do not retain the `status` pointer.

Only one callback is stored. Registering another callback replaces the existing
one. Passing `NULL` unregisters it.

## RSSI And Scan

`wifi_manager_get_rssi()` is valid only after the Station has an IPv4 address.
RSSI is expressed in dBm; values closer to zero indicate a stronger signal.

`wifi_manager_scan_and_log()` performs an active all-channel scan with hidden
SSIDs enabled. It blocks the calling task until scanning finishes, dynamically
allocates the AP result array, prints each result, and then releases both the
application buffer and driver-owned scan list.

Do not call the scan API from a Wi-Fi/IP event callback or the LVGL task.

## Important Current Limitations

- `wifi_manager_disconnect()` is declared but not implemented.
- There is no public deinit API.
- There is no automatic reconnect or retry backoff after disconnection.
- `IP_EVENT_STA_GOT_IP` updates state to `CONNECTED` and notifies the
  application callback.
- `IP_EVENT_STA_LOST_IP` clears the address/RSSI, transitions an associated
  station to `WAITING_FOR_IP`, and notifies the application callback.
- Credential validation in `wifi_manager_connect()` needs review: the current
  SSID null/empty condition uses `||`, which can accept an empty SSID and can
  dereference a NULL SSID pointer.
- `wifi_manager_is_connected()` should only be called after successful init.
  Its current precondition macro returns an `esp_err_t` value from a `bool`
  function when uninitialized, which can produce a truthy result.
- Some status writes in connect/init paths are not protected by
  `s_status_lock`; event-loop reads/writes should be reviewed as concurrency
  grows.
- No connection timeout moves a long-running `CONNECTING` or
  `WAITING_FOR_IP` state to `FAILED`.

## Expected Logs

Typical successful startup includes:

```text
I (...) WIFI_MANAGER: Wi-Fi manager initialized: mode=STATION, storage=RAM
I (...) WIFI_MANAGER: Connecting to Wi-Fi SSID: ...
I (...) WIFI_MANAGER: Event: WIFI_EVENT_STA_CONNECTED
I (...) WIFI_MANAGER: Waiting for IPv4 address
I (...) WIFI_MANAGER: Event: IP_EVENT_STA_GOT_IP, address=...
```

On disconnection, the ESP-IDF reason code is stored in
`wifi_manager_status_t.disconnect_reason` and the state becomes
`WIFI_MANAGER_STATE_DISCONNECTED`.

## Future Attention

- Implement disconnect and a symmetric deinit path.
- Fix SSID validation and the pre-init boolean return behavior.
- Notify status after `GOT_IP` and update status after `LOST_IP`.
- Protect all shared status mutations consistently.
- Add bounded reconnect with backoff and a connection timeout.
- Move credential persistence to the future configuration/NVS owner.
- Add BLE provisioning without keeping BLE active after successful setup.
- Replace scan logging with a structured result API only when the UI needs AP
  selection.
