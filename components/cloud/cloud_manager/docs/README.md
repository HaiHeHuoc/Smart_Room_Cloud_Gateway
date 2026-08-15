# `cloud_manager` Component

## Purpose

`cloud_manager` owns authenticated latest-value telemetry upload to Firebase
Realtime Database. It receives copied sensor and application-composed audio
snapshots through a length-one queue, waits for network readiness, obtains a
valid Firebase ID token from `firebase_auth`, and performs HTTPS REST `PUT`
operations from its own task.

It does not read the DHT22, own `audio_manager`, or call LVGL.

## Version 1 Status

```text
Release: v1.0.0
Status: Implemented and hardware accepted
```

## Implemented Behavior

- One cloud FreeRTOS task with task-owned HTTPS/TLS lifecycle.
- Length-one queue using `xQueueOverwrite()` to retain the newest telemetry.
- Application-composed audio snapshot included with each telemetry update.
- Retained IPv4 readiness and a non-zero network epoch.
- Task-notification wakeups for telemetry and network edges.
- On-demand sign-in and token refresh through `firebase_auth`.
- Authenticated Realtime Database requests using a Firebase ID token.
- Bounded JSON and authenticated-URL buffers.
- HTTP 2xx success handling, including Firebase `204 No Content`.
- Explicit classification of network, transport, HTTP, authentication,
  configuration, and internal failures.
- HTTP-client invalidation after network-epoch or token-generation changes.
- Bounded 401 recovery and one forced-token recovery for HTTP 403.
- Retry backoff from 5 seconds to a maximum of 60 seconds.
- Thread-safe status snapshots and upload counters.
- Status callbacks invoked after releasing component mutexes.
- Zeroization of token-bearing URL and request data at the end of ownership.

## Public API

| API | Role |
|---|---|
| `cloud_manager_init()` | Validate/copy configuration and create queue/state |
| `cloud_manager_start()` | Start the single upload task |
| `cloud_manager_register_status_callback()` | Register one short non-blocking status callback |
| `cloud_manager_notify_network_state()` | Retain IPv4 state, advance network epoch, and wake task |
| `cloud_manager_post_sensor_telemetry()` | Replace pending sensor/audio data with the newest finite snapshot |
| `cloud_manager_get_status()` | Copy cloud state, counters, HTTP status, and retry delay |

## Initialization

Initialize Authentication and cloud state before the sensor producer. Start the
cloud task only after the network coordinator permits the handoff:

```c
ESP_ERROR_CHECK(firebase_auth_init(&auth_config));
ESP_ERROR_CHECK(cloud_manager_init(&cloud_config));
ESP_ERROR_CHECK(cloud_manager_register_status_callback(
    app_cloud_status_callback,
    NULL));

/* Start only after stored-Wi-Fi startup or completed provisioning adoption. */
ESP_ERROR_CHECK(cloud_manager_start());
```

The configured URL must be a `.json` endpoint without query parameters:

```c
const cloud_manager_config_t config = {
    .firebase_latest_url =
        "https://<database>.<region>.firebasedatabase.app/"
        "devices/esp32s3-001/latest.json",
    .publish_period_ms = 10000U,
};
```

`cloud_manager` adds the current Firebase ID token and `print=silent`
internally.

## Upload Flow

```text
sensor callback
    -> audio_manager_get_status()
    -> map audio_manager state to cloud-owned audio state
    -> cloud_manager_post_sensor_telemetry()
    -> overwrite length-one queue
    -> wake cloud task

Wi-Fi callback
    -> cloud_manager_notify_network_state(has_ipv4)
    -> retain edge and advance network epoch
    -> wake cloud task

cloud task
    -> discard client from an obsolete network/token generation
    -> firebase_auth_get_valid_id_token()
    -> serialize sensor data plus nested audio object
    -> HTTPS PUT latest.json with Firebase ID token
    -> classify result
    -> reset, retry, latch error, or publish Online status
```

Current payload:

```json
{
  "temperature_c": 30.1,
  "humidity_percent": 64.5,
  "sensor_valid": true,
  "sensor_stale": false,
  "sensor_state": 3,
  "last_error": 0,
  "sample_uptime_ms": 123456,
  "audio": {
    "state": "recording",
    "recording": true,
    "playback": false,
    "last_error": 0
  },
  "source": "esp32_cloud_manager"
}
```

The nested audio object follows these rules:

- `state` is the source of truth and is serialized as one of `unavailable`,
  `ready`, `idle`, `recording`, `processing`, `playback`, or `error`.
- `recording` is true only while `state` is `recording`.
- `playback` is true only while `state` is `playback`.
- `last_error` is copied from `audio_manager_status_t` when its status snapshot
  is available. If the manager is not initialized/available yet, the state is
  `unavailable` and the status-query error is retained instead.

Audio status is sampled when the sensor callback composes a new latest-value
telemetry snapshot. It therefore follows the existing cloud/sensor publish
pipeline; changing audio state does not directly start an HTTPS/TLS request.
Short audio transitions can be absent from Firebase if they occur entirely
between telemetry snapshots.

Consumers must inspect `sensor_valid` and `sensor_stale`; not every finite
number represents a valid physical reading.

## State And Retry Policy

| State / result | Behavior |
|---|---|
| `WAITING_FOR_NETWORK` | Retain telemetry and wait for IPv4 notification |
| `WAITING_FOR_DATA` | Network ready, no pending telemetry |
| `UPLOADING` | Authentication or HTTPS request in progress |
| HTTP 2xx | Enter `ONLINE`, clear backoff, retain client only if generations match |
| Transport / DNS / TLS / timeout | Reset client and enter bounded `RETRY_WAIT` |
| HTTP 408, 429, 5xx | Reset client and retry with bounded backoff |
| HTTP 401 | Invalidate ID token and schedule bounded recovery |
| HTTP 403 | One fresh-token recovery, then terminal `AUTH_ERROR` if still rejected |
| Credential rejection | Enter terminal `AUTH_ERROR`; no hot sign-in loop |
| Deterministic config/internal failure | Enter terminal `ERROR` |

A newer telemetry snapshot replaces queued data while an older snapshot is
being retried. A successful request clears only the snapshot actually uploaded.

## Threading And Memory

- Only the cloud task performs authentication and HTTPS work.
- Sensor and Wi-Fi callbacks allocate no memory and do no network I/O.
- The sensor callback obtains a bounded `audio_manager` status snapshot and
  copies only cloud-owned audio state/error fields into telemetry.
- `cloud_manager` does not depend on `audio_manager` headers or own I2S/audio
  lifecycle.
- The status callback runs in cloud-task context and must remain short.
- HTTP clients are reused only across successful uploads with unchanged network
  and token generations.
- Network edges invalidate earlier TLS sessions.
- The component is one-shot and has no stop/deinit API.

## Firebase Setup And Security

Use:

- [`firebase_auth` setup and security guide](../../firebase_auth/docs/FIREBASE_SETUP_AND_SECURITY.md)
- [`firebase_auth` component contract](../../firebase_auth/docs/README.md)
- [`project setup`](../../../../docs/SETUP.md)

Never store administrator credentials, service-account private keys, database
secrets, ID tokens, or refresh tokens in firmware or source control.
Realtime Database rules must deny anonymous access and restrict the dedicated
UID to its intended device path.

## Future Attention

- Make database host, device ID, and publish interval deployment configuration.
- Add event-driven audio-state publishing only if Firebase must observe every
  short audio transition and the additional network load is justified.
- Add bounded SD-backed history only when required.
- Add stop/deinit/restart APIs only when runtime lifecycle requirements exist.
- Define fleet identity, rotation, revocation, and abuse controls before a
  production deployment.
