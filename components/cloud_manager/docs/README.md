# cloud_manager Component Notes

## Purpose

`cloud_manager` owns periodic authenticated Firebase Realtime Database uploads
for the latest sensor telemetry. It receives copied sensor snapshots through a
latest-value queue, waits for Wi-Fi, obtains a valid Firebase ID token from
`firebase_auth`, and performs an HTTPS REST `PUT` from its own task.

The component does not read the DHT22 and does not update LVGL. Those concerns
remain owned by `sensor_manager` and `app_gui`.

## What Is Implemented

- One 12 KB FreeRTOS cloud task at priority 4.
- A queue of length one using `xQueueOverwrite()`, retaining only the newest
  telemetry snapshot.
- Wi-Fi readiness checks through `wifi_manager_is_connected()`.
- On-demand sign-in or token refresh through
  `firebase_auth_get_valid_id_token()`.
- Authenticated Firebase URLs using `auth=<ID_TOKEN>&print=silent`.
- JSON encoding into a bounded local buffer.
- HTTPS certificate verification through the ESP certificate bundle.
- HTTP 2xx acceptance, including Firebase's expected `204 No Content` when
  `print=silent` is used.
- Retry backoff starting at 5 seconds and capped at 60 seconds.
- Thread-safe status snapshots and upload counters.
- A single status callback invoked after releasing the status mutex, suitable
  for forwarding copied snapshots to the application GUI queue.
- A transmit buffer sized for the long request line containing the ID token.

## Initialization And Use

Initialize networking, Wi-Fi, and Firebase Authentication before starting the
cloud task:

```c
ESP_ERROR_CHECK(firebase_auth_init(&auth_config));
ESP_ERROR_CHECK(cloud_manager_init(&cloud_config));
ESP_ERROR_CHECK(cloud_manager_register_status_callback(
    app_cloud_status_callback,
    NULL));
ESP_ERROR_CHECK(cloud_manager_start());
```

The configured Firebase URL must be a base `.json` URL without query
parameters:

```c
const cloud_manager_config_t cloud_config = {
    .firebase_latest_url =
        "https://<project>-default-rtdb.<region>.firebasedatabase.app/"
        "devices/esp32s3-001/latest.json",
    .publish_period_ms = 10000U,
};
```

Post telemetry from the sensor callback without waiting:

```c
cloud_sensor_telemetry_t telemetry = {
    .temperature_c = status->temperature_c,
    .humidity_percent = status->humidity_percent,
    .data_valid = status->data_valid,
    .data_stale = status->data_stale,
    .sensor_state = (int32_t)status->state,
    .last_error = status->last_error,
    .sample_uptime_ms = status->last_success_time_ms,
};

esp_err_t ret = cloud_manager_post_sensor_telemetry(&telemetry);
```

## Public API

| API | Current role |
| --- | --- |
| `cloud_manager_init()` | Validate/copy configuration and create the queue and status mutex. |
| `cloud_manager_start()` | Start the single cloud upload task. |
| `cloud_manager_register_status_callback()` | Register or remove the single non-blocking status callback. |
| `cloud_manager_post_sensor_telemetry()` | Replace pending data with the newest finite telemetry snapshot. |
| `cloud_manager_get_status()` | Copy cloud state, counters, latest HTTP status, and retry delay. |

## Upload Flow

```text
sensor callback
    -> cloud_manager_post_sensor_telemetry()
    -> latest-value queue
    -> cloud task waits for Wi-Fi
    -> firebase_auth_get_valid_id_token()
    -> HTTPS PUT latest.json?auth=<token>&print=silent
    -> status/counters/backoff update
    -> status callback forwards a copied snapshot after mutex release
```

Successful payloads currently contain:

```json
{
  "temperature_c": 30.1,
  "humidity_percent": 64.5,
  "sensor_valid": true,
  "sensor_stale": false,
  "sensor_state": 3,
  "last_error": 0,
  "sample_uptime_ms": 123456,
  "source": "esp32_cloud_manager"
}
```

Temperature and humidity are serialized exactly as posted. A failed sensor
read currently reaches Firebase as the finite `-1.0` sentinel together with
`last_error`; `sensor_valid` and `sensor_stale` remain separate metadata based
on the sensor manager's successful-sample history. Firebase consumers must
inspect those metadata fields and must not treat every numeric value as a valid
physical measurement.

## State And Retry Behavior

- `WAITING_FOR_NETWORK`: telemetry is pending but Wi-Fi has no IPv4 address.
- `WAITING_FOR_DATA`: Wi-Fi is ready but no telemetry is pending.
- `UPLOADING`: authentication or HTTPS upload is running.
- `ONLINE`: the latest upload returned HTTP 2xx.
- `RETRY_WAIT`: transport errors, HTTP 408/429, or HTTP 5xx use exponential
  backoff.
- `AUTH_ERROR`: Authentication failure or Firebase HTTP 401/403.
- `ERROR`: a non-retryable HTTP/configuration failure.

Pending telemetry is retained after a failure. A newer queued sensor snapshot
replaces it before the next attempt.

## Memory And Threading Notes

- Network I/O runs only in the cloud task, never in the sensor callback.
- `cloud_manager_post_sensor_telemetry()` is non-blocking.
- The status callback runs in the cloud task context. It must remain short and
  must not perform LVGL operations; `main` forwards it through the GUI queue.
- The authenticated URL can exceed ESP HTTP Client's default 512-byte TX
  buffer. The component explicitly sizes `buffer_size_tx` for the token URL.
- ID-token and authenticated-URL buffers are static; the HTTP TX buffer exists
  only for the lifetime of each HTTP client.
- There is no stop/deinit API. Initialization and task start are one-shot.
- Invalid/stale telemetry is not converted to JSON `null` and is not omitted;
  finite sentinel values are uploaded as supplied.

## Firebase Setup

Project identifiers, UID, REST test steps, and example Database Security Rules
are recorded in `components/cloud_manager/README.txt`. The authenticated host
test is `Test/TestFirebase_Auth.ps1`.

Never store Firebase administrator credentials, service-account private keys,
ID tokens, or refresh tokens in firmware or source control.

## Future Attention

- Move device email/password out of shared source and into provisioning or
  protected local configuration.
- Add schema validation rules after the final telemetry format is stable.
- Decide whether invalid/stale temperature and humidity should remain numeric,
  become JSON `null`, or move into a separate last-known-good field.
- Add stop/deinit only if runtime service shutdown becomes necessary.
