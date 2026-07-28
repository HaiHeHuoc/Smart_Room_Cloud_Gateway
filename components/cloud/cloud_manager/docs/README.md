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
- A retained IPv4-ready flag and non-zero network epoch fed by the existing
  Wi-Fi status callback.
- Task-notification wakeups for network edges and new telemetry; retry and
  publish waits no longer use an unconditional long delay.
- On-demand sign-in or token refresh through
  `firebase_auth_get_valid_id_token()`.
- Authenticated Firebase URLs using `auth=<ID_TOKEN>&print=silent`.
- JSON encoding into a bounded local buffer.
- HTTPS certificate verification through the ESP certificate bundle.
- HTTP 2xx acceptance, including Firebase's expected `204 No Content` when
  `print=silent` is used.
- Explicit attempt classification for network, transport, HTTP,
  authentication, configuration, and internal failures.
- HTTP-client reset on every network epoch, failed transport, retryable HTTP
  response, rejected authenticated identity, or token-generation change.
- Bounded 401 recovery and one forced token recovery for a persistent 403.
- Retry backoff starting at 5 seconds and capped at 60 seconds.
- Thread-safe status snapshots and upload counters.
- A single status callback invoked after releasing the status mutex, suitable
  for forwarding copied snapshots to the application GUI queue.
- A transmit buffer sized for the long request line containing the ID token.

## Initialization And Use

Initialize Firebase Authentication and the cloud state/queue before starting
the sensor producer. Start the task only after the application network
coordinator reports a safe handoff:

```c
ESP_ERROR_CHECK(firebase_auth_init(&auth_config));
ESP_ERROR_CHECK(cloud_manager_init(&cloud_config));
ESP_ERROR_CHECK(cloud_manager_register_status_callback(
    app_cloud_status_callback,
    NULL));

/* sensor_manager may now post snapshots into the initialized queue. */

if (network_state == APP_NETWORK_COORDINATOR_STATE_CONNECTING ||
    network_state == APP_NETWORK_COORDINATOR_STATE_ONLINE) {
    ESP_ERROR_CHECK(cloud_manager_start());
}
```

The component may be initialized while BLE provisioning is active, but the
application defers `cloud_manager_start()` until the network coordinator
reaches `CONNECTING` for stored credentials or `ONLINE` after provisioning.
This avoids allocating the 12 KB cloud task stack while temporary BLE
resources are active. The Wi-Fi callback does not publish application-ready
IPv4 until provisioning cleanup and active-connection adoption complete.

Telemetry may be posted after `cloud_manager_init()` and before task start.
The length-one queue retains the latest local sensor snapshot during
provisioning or a provisioning timeout without triggering authentication or
HTTPS activity.

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
| `cloud_manager_notify_network_state()` | Retain an IPv4 edge, advance the non-zero network epoch, and wake the task without waiting. |
| `cloud_manager_post_sensor_telemetry()` | Replace pending data with the newest finite telemetry snapshot and wake the task. |
| `cloud_manager_get_status()` | Copy cloud state, counters, latest HTTP status, and retry delay. |

## Upload Flow

```text
sensor callback
    -> cloud_manager_post_sensor_telemetry()
    -> latest-value queue
    -> wake cloud task
Wi-Fi callback
    -> cloud_manager_notify_network_state(has_ipv4)
    -> retain state + advance epoch on an edge
    -> wake cloud task
cloud task
    -> compare the observed epoch with the client epoch
    -> discard any client from an older epoch
    -> firebase_auth_get_valid_id_token()
    -> HTTPS PUT latest.json?auth=<token>&print=silent
    -> classify result and apply reset/retry/auth policy
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
- `AUTH_ERROR`: credentials are rejected, token invalidation fails, or a fresh
  token is still rejected by HTTP 403.
- `ERROR`: a deterministic configuration or internal lifecycle failure.

Pending telemetry is retained after a failure. A newer queued sensor snapshot
replaces it before the next attempt.

An upload attempt has one explicit result class:

| Attempt result | Cloud behavior |
| --- | --- |
| Success / HTTP 2xx | Enter `ONLINE`, reset backoff, and reuse the client only while network and token generations stay unchanged. |
| No IPv4 | Reset the client and wait indefinitely for a network notification. |
| Transport, DNS, socket, TLS, timeout, or HTTP status 0 from a real request | Reset the client and enter bounded `RETRY_WAIT`. |
| HTTP 408, 429, or 5xx | Reset the client and enter bounded `RETRY_WAIT`. |
| HTTP 401 | Reset the client, invalidate the current ID token, and schedule bounded fresh-token recovery. |
| HTTP 403 | Perform at most one forced token recovery; a fresh rejected token enters `AUTH_ERROR`. |
| Firebase credential error | Enter terminal `AUTH_ERROR`; do not hot-loop sign-in. |
| Invalid arguments, URL/payload size, or internal lifecycle error | Enter terminal `ERROR`; do not infer retryability merely from HTTP status zero. |

The task waits with FreeRTOS task notifications. Telemetry wakes it so the
newest value can be drained, but does not shorten an active failure deadline.
An offline-to-online network edge invalidates the old transport, resets the
retry delay to 5 seconds, and permits a prompt attempt. Delay accounting uses
wrap-safe tick subtraction, never requests a zero-tick wait, doubles without
overflow, and caps at 60 seconds.

## HTTP Client Reset Matrix

Only the cloud task initializes, configures, performs, resets, or cleans the
single HTTP client.

| Condition | Reset client? | Next action |
| --- | --- | --- |
| HTTP 2xx, unchanged network epoch and token generation | No | Reuse at normal publish cadence |
| Online/offline edge, even if reconnect finishes during backoff | Yes | Re-evaluate latest network state |
| DNS/socket/TLS/timeout/other transport failure | Yes | Backoff retry |
| HTTP status 0 after a real perform failure | Yes | Backoff retry |
| HTTP 408, 429, or 5xx | Yes | Backoff retry |
| HTTP 401 | Yes | Invalidate ID token and recover with backoff |
| HTTP 403 forced recovery | Yes | One fresh-token attempt, then `AUTH_ERROR` if rejected |
| Token generation changes | Yes | Recreate with the new authenticated identity |
| Deterministic request/configuration error | Yes when a handle exists | Latch `ERROR` |

Cleanup always nulls the handle even when ESP-IDF reports a cleanup error.
Obsolete token, authenticated URL, client URL, and payload copies are securely
cleared where their lifetime ends. None of those sensitive values is logged.

## Memory And Threading Notes

- Network I/O runs only in the cloud task, never in the sensor callback.
- `cloud_manager_post_sensor_telemetry()` and
  `cloud_manager_notify_network_state()` are non-blocking and allocate no
  memory.
- The status callback runs in the cloud task context. It must remain short and
  must not perform LVGL operations; `main` forwards it through the GUI queue.
- The authenticated URL can exceed ESP HTTP Client's default 512-byte TX
  buffer. The component explicitly sizes `buffer_size_tx` for the token URL.
- The cloud task reuses one HTTP client handle only across successful uploads
  with unchanged network and authentication generations.
- ID-token, authenticated-URL, and request-payload buffers are static. The
  payload must remain valid because ESP HTTP Client retains its pointer between
  calls to `esp_http_client_perform()`.
- A transport or retryable HTTP failure records non-sensitive diagnostics,
  fully cleans up the current client, and lets the next retry create a clean
  transport.
- Every online/offline edge advances the network epoch. This catches a complete
  disconnect/reconnect cycle that occurs during a long retry wait and prevents
  reuse of the earlier TLS connection.
- The length-one queue and a task-local pending snapshot have distinct
  ownership. A successful request clears only the snapshot it uploaded; a
  newer queue value posted during HTTPS remains available for the next loop.
- There is no stop/deinit API. Initialization and task start are one-shot.
- Invalid/stale telemetry is not converted to JSON `null` and is not omitted;
  finite sentinel values are uploaded as supplied.

## Firebase Setup

Project identifiers, UID, REST test steps, and example Database Security Rules
are recorded in `components/cloud/cloud_manager/README.txt`. The authenticated host
test is `Test/TestFirebase_Auth.ps1`.

Never store Firebase administrator credentials, service-account private keys,
ID tokens, or refresh tokens in firmware or source control.

## Phase 4 Acceptance

Phase 4 was hardware-accepted on 2026-07-19. Authentication, authenticated
telemetry upload, latest-data visibility in Firebase, LCD Cloud state updates,
and failure/retry behavior were all verified on the ESP32-S3 target.

The deferred task-start integration added by checkpoint 6.3.2 was
hardware-accepted on 2026-07-26 using timeout, reset, reprovisioning, Wi-Fi
adoption, Firebase upload, and GUI cloud-state recovery.

Checkpoint 6.3.4 initializes this component before the sensor producer and
keeps task creation behind the coordinator handoff gate. The implementation is
build-verified; its provisioning, local-sensor, watchdog, reconnect, and
Firebase recovery paths still require hardware smoke testing.

## Phase 6.4.6 Status

**IMPLEMENTED / HARDWARE TEST PENDING**

The cloud task now observes explicit network epochs, wakes on important events,
invalidates stale transports, classifies every attempt, and applies bounded
transport and authentication recovery without creating another task or Wi-Fi
callback. Static build validation does not prove runtime Wi-Fi, TLS, Firebase,
heap, or token-refresh behavior.

## Phase 6.4.6 Manual Hardware Matrix

- **Test A - Normal upload:** verify `Wait -> Sync -> Online` and the newest
  sample in Firebase.
- **Test B - Wi-Fi disconnect/reconnect:** disable then enable the hotspot;
  verify `WAITING_FOR_NETWORK`, a new network epoch/client reset, then
  `Sync -> Online`.
- **Test C - Complete reconnect during long backoff:** reach a near-60-second
  retry, disconnect and reconnect before it expires; verify an immediate wake,
  epoch change, stale-client discard, and prompt upload.
- **Test D - Transport/TLS failure:** inject one timeout/socket/TLS failure;
  verify `RETRY_WAIT`, reset, bounded retry, then `ONLINE`.
- **Test E - HTTP 500/429:** verify `RETRYABLE_HTTP`, client reset, bounded
  backoff, and upload of the newest telemetry.
- **Test F - HTTP 401:** reject the active ID token; verify invalidation,
  client reset, bounded authentication recovery, then `ONLINE`.
- **Test G - Persistent 403 or invalid credentials:** verify at most one forced
  recovery, terminal `AUTH_ERROR`, no rapid sign-in, and no secret logs.
- **Test H - Deterministic malformed configuration:** inject invalid URL,
  argument, or size; verify terminal `ERROR` without endless retry.
- **Test I - Latest telemetry during retry/request:** post several samples
  during failure and active HTTPS; verify only the newest pending value follows
  the attempted snapshot.
- **Test J - Provisioning regression:** erase Wi-Fi config and exercise one or
  more Phase 6.4.5 sessions; verify no cloud task during BLE/retry, one start
  after successful adoption, and a successful first upload.
- **Test K - Long-running recovery:** run at least two hours with reconnects,
  token refresh, injected HTTP failures, and continuous sensor updates; verify
  no watchdog, stack warning, duplicate task/client, stale transport, or
  downward heap trend.

## Future Attention

- Move device email/password out of shared source and into provisioning or
  protected local configuration.
- Add schema validation rules after the final telemetry format is stable.
- Decide whether invalid/stale temperature and humidity should remain numeric,
  become JSON `null`, or move into a separate last-known-good field.
- Add stop/deinit only if runtime service shutdown becomes necessary.
- Terminal `AUTH_ERROR` and `ERROR` are intentionally latched for this
  one-shot service lifetime; runtime credential/configuration replacement and
  an operator-triggered restart API remain out of scope.

## Long-Running Recovery Test

After changing HTTP/TLS lifecycle code, run the firmware for at least two
hours, including the ID-token refresh window. Confirm periodic uploads continue
and the LCD returns from `Cloud: Retry` to `Cloud: Online` after a temporary
network failure. If a transport failure occurs, retain the complete
`HTTP transport error` log containing `socket_errno`, `tls_error`, and
`tls_flags` for diagnosis.
