# sensor_manager Component Notes

## Purpose

`sensor_manager` owns periodic DHT22 sampling and converts raw read results into
a thread-safe application status snapshot. It publishes a numeric failure
sentinel immediately after an unsuccessful read, retains the timestamp of the
latest successful sample for stale-state classification, and notifies one
application callback after each read attempt.

## What Is Implemented

- Copies a sampling configuration during one-time initialization.
- Enforces a minimum 2000 ms sample period.
- Requires the stale timeout to be greater than the sample period.
- Starts one 3072-byte FreeRTOS sampling task at priority 4.
- Waits 2000 ms before the first sensor read.
- Uses `vTaskDelayUntil()` to avoid accumulating scheduling drift.
- Protects status and callback snapshot reads with a mutex; callback
  registration is restricted to the pre-start lifecycle window.
- Tracks successful, failed, and consecutive failed read counts.
- Publishes `-1.0f` for temperature and humidity after a read failure.
- Retains the latest successful-sample timestamp for degraded/stale decisions.
- Reports `READY`, `DEGRADED`, or `ERROR` based on data availability and age.
- Invokes the application callback after releasing the internal mutex.

## State Model

```text
UNINITIALIZED
    -> INITIALIZED
    -> RUNNING
    -> READY       after a successful read
    -> DEGRADED    after a failure while the latest success is still recent
    -> ERROR       when no successful data exists or the latest success is stale
```

A later successful read returns the state to `READY` and resets the consecutive
failure count.

## Initialization And Use

```c
const sensor_manager_config_t config = {
    .sample_period_ms = 2000U,
    .stale_timeout_ms = 10000U,
};

ESP_ERROR_CHECK(sensor_manager_init(&config));
ESP_ERROR_CHECK(
    sensor_manager_register_callback(app_sensor_status_callback, NULL));
ESP_ERROR_CHECK(sensor_manager_start());
```

Callback registration must happen after initialization and before the task is
started. Every queue or service used by the callback must be initialized before
`sensor_manager_start()` because the producer is independent of network state
and may publish as soon as its initial sensor delay completes.

## Public API

| API | Current role |
| --- | --- |
| `sensor_manager_init()` | Validate/copy configuration and create the status mutex. |
| `sensor_manager_register_callback()` | Register the single non-NULL callback/context pair before start. |
| `sensor_manager_start()` | Start periodic DHT22 sampling once. |
| `sensor_manager_get_status()` | Copy the latest status snapshot under the mutex. |

## Callback And Thread Safety

The callback executes in the sensor manager task. It receives a temporary
stack snapshot, so the pointer must not be retained. Application callbacks
should return quickly; the current project maps the snapshot and posts it to
`app_gui`, then posts a second copied representation to `cloud_manager`. The
callback does not touch LVGL or perform network I/O directly.

The internal mutex is released before application callback code runs. This
prevents callback code from blocking status readers while it processes a
snapshot.

In the current composition, both the GUI queue and the cloud latest-value
queue exist before this task starts. Sampling therefore continues locally
during Wi-Fi connection, BLE provisioning, provisioning timeout, and network
failure. Posting telemetry only copies a snapshot; it does not start Firebase
Authentication or TLS from the sensor task.

## Data-Quality Behavior

- A successful read stores new values, sets `data_valid`, clears `data_stale`,
  records `ESP_OK`, and enters `READY`.
- A failed read publishes `-1.0f` for temperature and humidity and records the
  driver error.
- A failure becomes `DEGRADED` while the latest successful sample is younger
  than the configured stale timeout.
- Missing or old successful data enters `ERROR` and sets `data_stale`.
- `data_valid` records whether any successful sample exists; it does not make
  the `-1.0f` failure sentinel a valid physical reading.
- Staleness is currently recalculated when a read fails; `get_status()` does
  not independently age the snapshot at read time.

## Current Limitations

- There is no stop, restart, or deinit API.
- Only one callback is supported, and `NULL` cannot be used to unregister it.
- Lifecycle flags assume initialization/start are controlled by one
  application context.
- The task handle is retained but not yet exposed or used for shutdown.
- `-1.0f` is also a physically possible temperature. Consumers must use
  `last_error`, `data_valid`, and `data_stale`, not the numeric sentinel alone,
  when deciding whether data is usable.

## Expected Logs

Normal startup includes initialization and task-start messages. Successful
samples are logged at debug level; sensor read failures are warnings.

## Future Attention

- Add stop/deinit only when runtime service shutdown is required.
- Recalculate staleness in `sensor_manager_get_status()` if callers require
  wall-clock-accurate aging even when sampling stops.
- Add retry/backoff policy only if DHT22 failure behavior justifies it.
