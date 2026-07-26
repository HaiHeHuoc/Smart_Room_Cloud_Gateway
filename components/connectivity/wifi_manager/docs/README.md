# wifi_manager Component Notes

## Purpose

`wifi_manager` owns the ESP32-S3 Wi-Fi Station driver and its connection
status. It creates the default Station network interface, applies credentials,
handles Wi-Fi/IP events, performs bounded automatic reconnect, and exposes
thread-safe status snapshots to the application and GUI.

Credentials are supplied by the application and stored by the ESP-IDF Wi-Fi
driver in RAM through `WIFI_STORAGE_RAM`. Provisioning and persistent
credential ownership remain outside this component.

## What Is Implemented

- Initializes and starts the ESP-IDF Wi-Fi driver in Station mode.
- Creates the default ESP-NETIF Station interface.
- Registers handlers for all `WIFI_EVENT` and `IP_EVENT` events.
- Tracks SSID, state, IPv4 address, RSSI validity, and disconnect reason.
- Protects shared status and reconnect state with a FreeRTOS critical section.
- Supports one application status callback with optional user data.
- Performs automatic reconnect using a dedicated task and task notifications.
- Uses exponential retry delays of 1, 2, 4, 8, 16, then at most 30 seconds.
- Resets retry delay and attempt count after `IP_EVENT_STA_GOT_IP`.
- Supports manual disconnect and suppresses automatic reconnect afterward.
- Applies a 30-second connection/DHCP timeout to every initial or automatic
  call to `esp_wifi_connect()`.
- Reads connected AP RSSI with `esp_wifi_sta_get_rssi()`.
- Performs a blocking active scan and logs nearby access points.
- Cleans up partially initialized Wi-Fi, event, task, timer, and netif
  resources when initialization fails.

## State Model

Normal startup follows:

```text
UNINITIALIZED
    -> READY
    -> CONNECTING
    -> WAITING_FOR_IP
    -> CONNECTED
```

Reconnect follows:

```text
CONNECTING or WAITING_FOR_IP
    -> DISCONNECTED
    -> RETRY_WAIT
    -> CONNECTING
```

If `esp_wifi_connect()` fails synchronously, the current attempt moves to
`FAILED`. An automatic reconnect attempt then re-enters the existing retry
flow. `CONNECTED` means the Station has obtained an IPv4 address; association
alone is represented by `WAITING_FOR_IP`.

The ESP-IDF reason from `WIFI_EVENT_STA_DISCONNECTED` is preserved in
`wifi_manager_status_t.disconnect_reason`. A connection timeout does not add a
synthetic public reason code; the subsequent driver disconnect event remains
the source of the reason.

## Connection And DHCP Timeout

One private one-shot `esp_timer` covers each complete connection attempt:

```text
arm 30-second timer
    -> esp_wifi_connect()
    -> access-point association
    -> DHCP wait
    -> IP_EVENT_STA_GOT_IP stops timer
```

The timer is armed for both the initial connection and every automatic retry.
It is canceled when the attempt obtains an IPv4 address, disconnects, is
manually canceled, or fails synchronously.

The timer callback intentionally does very little. It stores a protected
timeout flag and attempt generation, then notifies the existing reconnect task.
It does not call Wi-Fi APIs, invoke the application callback, block, or perform
recovery itself.

The reconnect task rechecks all timeout conditions under the component lock.
For a current timed-out attempt it logs a warning and calls
`esp_wifi_disconnect()` outside the lock. Normally the resulting
`WIFI_EVENT_STA_DISCONNECTED` schedules the existing exponential-backoff retry.
If the driver reports that the attempt cannot be aborted, the reconnect task
schedules the same retry path directly.

### Race Protection

Each attempt has an internal generation number and absolute deadline. These
values prevent a delayed timer callback from attempt N from aborting attempt
N+1. The timeout flag is accepted only while all of these remain true:

- the manager and credentials are valid;
- automatic reconnect is enabled;
- manual disconnect has not been requested;
- no IPv4 address is present;
- the same generation is still active;
- state is `CONNECTING` or `WAITING_FOR_IP`;
- the stored deadline has expired.

`GOT_IP`, disconnect, and manual disconnect invalidate the active attempt under
the same lock. If timeout recovery has already claimed the attempt, a racing
`GOT_IP` event is ignored so the abort can finish without briefly publishing a
false `CONNECTED` state. Duplicate task notifications are harmless because the
task rechecks state, generation, and policy before starting another attempt.

## Retry And Manual Disconnect

Unexpected `WIFI_EVENT_STA_DISCONNECTED` transitions through `DISCONNECTED`,
publishes that status, then moves to `RETRY_WAIT` and wakes the reconnect task.
The task delays using the current backoff value, rechecks policy, reports
`CONNECTING`, and starts the next timed attempt.

`wifi_manager_disconnect()` records manual intent before calling the driver,
disables automatic reconnect, cancels the active timeout, and prevents a task
waiting in backoff from starting another attempt. If timeout recovery already
owns the driver abort, manual disconnect joins that in-progress operation
instead of issuing a duplicate disconnect call.

A later successful call to `wifi_manager_connect()` supplies credentials again,
clears manual-disconnect intent, resets backoff to one second, and enables
automatic reconnect.

## Initialization Order

The shared ESP-IDF network platform must exist before `wifi_manager_init()`:

```c
ESP_ERROR_CHECK(nvs_flash_init());
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(wifi_manager_init());
```

In this project the first three operations are owned by
`network_platform_init()`.

## Basic Usage

Register the status callback before connecting if the application must observe
the first `CONNECTING` state:

```c
ESP_ERROR_CHECK(
    wifi_manager_register_status_callback(
        app_wifi_status_callback,
        NULL));

const wifi_manager_sta_config_t config = {
    .ssid = "YOUR_SSID",
    .password = "YOUR_PASSWORD",
};

ESP_ERROR_CHECK(wifi_manager_connect(&config));
```

The config and its strings only need to remain valid for the duration of
`wifi_manager_connect()`. The component copies them into ESP-IDF's Station
configuration. An empty password is valid for an open network.

## Public API

| API | Current behavior |
| --- | --- |
| `wifi_manager_init()` | Initialize the Station driver, timer, event handlers, and reconnect task once. |
| `wifi_manager_connect()` | Validate/copy credentials and begin a timed asynchronous connection. |
| `wifi_manager_disconnect()` | Disconnect manually and suppress automatic reconnect. |
| `wifi_manager_get_status()` | Copy a locked snapshot of current manager status. |
| `wifi_manager_get_rssi()` | Read and cache RSSI while connected with IPv4. |
| `wifi_manager_is_connected()` | Return true only while wifi_manager owns a `CONNECTED` Station with valid IPv4. |
| `wifi_manager_register_status_callback()` | Register or replace the callback and immediately publish the current snapshot; pass NULL to unregister. |
| `wifi_manager_state_to_string()` | Convert a state enum to readable constant text. |
| `wifi_manager_scan_and_log()` | Block while scanning and print AP results. |

No timeout-specific public state or API is exposed.

## Status Callback And LVGL

The callback can run synchronously during registration/adoption or from the ESP
event loop task. It must return quickly and must not call LVGL directly.

```text
ESP Wi-Fi/IP event
    -> wifi_manager updates status
    -> application status callback
    -> app_gui_post_wifi_status()
    -> FreeRTOS queue
    -> app_gui task updates LVGL under its existing synchronization
```

The callback receives a temporary status snapshot. Copy required values before
returning and do not retain the pointer. Immediate publication during
registration prevents a GUI consumer from remaining stale if startup or
provisioning events happened before callback installation. Adoption of an
active provisioning connection republishes the final `CONNECTED` snapshot.

## Expected Logs

Successful startup includes messages similar to:

```text
I (...) WIFI_MANAGER: Wi-Fi manager initialized: mode=STATION, storage=RAM
I (...) WIFI_MANAGER: Connecting to configured Wi-Fi network
I (...) WIFI_MANAGER: Event: WIFI_EVENT_STA_CONNECTED
I (...) WIFI_MANAGER: Event: IP_EVENT_STA_GOT_IP, address=...
```

A timeout produces a warning similar to:

```text
W (...) WIFI_MANAGER: Wi-Fi connection/DHCP timed out after 30000 ms, generation=...
```

The subsequent disconnect and retry use the normal state callback and backoff
logs. Stale timeout callbacks are debug-level diagnostics.

## RSSI And Scan

`wifi_manager_get_rssi()` is valid only after the Station has an IPv4 address.
RSSI is expressed in dBm; values closer to zero indicate stronger signal.

`wifi_manager_scan_and_log()` performs a blocking active all-channel scan with
hidden SSIDs enabled. Do not call it from a Wi-Fi/IP event callback or the LVGL
task.

## Important Limitations And Future Attention

- The 30-second timeout is private and currently fixed at compile time.
- There is no public deinitialization API.
- Durable credentials remain owned by `config_manager`; `wifi_manager` keeps
  only the runtime Station configuration in driver RAM.
- `IP_EVENT_STA_LOST_IP` keeps the associated station in `WAITING_FOR_IP`; it
  does not start a new connection-attempt timer by itself.
- Runtime validation on hardware is still required for unreachable AP, wrong
  password, association without DHCP, repeated timeout/retry cycles, successful
  recovery, and manual disconnect during timeout or backoff.
- BLE transport lifecycle remains owned by `provisioning_manager`; a
  structured scan-result API remains future work.
