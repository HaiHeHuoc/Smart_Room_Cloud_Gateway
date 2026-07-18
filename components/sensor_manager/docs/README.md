# sensor_manager Component Notes

## Purpose

`sensor_manager` owns periodic DHT22 sampling and converts raw read results into
a thread-safe application status snapshot. It preserves the latest valid
sample across temporary failures and notifies one application callback after
each read attempt.

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
- Preserves the last-known-good values after a read failure.
- Reports `READY`, `DEGRADED`, or `ERROR` based on data availability and age.
- Invokes the application callback after releasing the internal mutex.

## State Model

```text
UNINITIALIZED
    -> INITIALIZED
    -> RUNNING
    -> READY       after a successful read
    -> DEGRADED    after a failure while retained data is still current
    -> ERROR       when no valid data exists or retained data is stale
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
started.

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

## Data-Quality Behavior

- A successful read stores new values, sets `data_valid`, clears `data_stale`,
  records `ESP_OK`, and enters `READY`.
- A failed read keeps the old temperature and humidity values.
- Retained data becomes `DEGRADED` while it is younger than the configured
  stale timeout.
- Missing or old retained data enters `ERROR` and sets `data_stale`.
- Staleness is currently recalculated when a read fails; `get_status()` does
  not independently age the snapshot at read time.

## Current Limitations

- There is no stop, restart, or deinit API.
- Only one callback is supported, and `NULL` cannot be used to unregister it.
- Lifecycle flags assume initialization/start are controlled by one
  application context.
- The task handle is retained but not yet exposed or used for shutdown.

## Expected Logs

Normal startup includes initialization and task-start messages. Successful
samples are logged at debug level; sensor read failures are warnings.

## Future Attention

- Add stop/deinit only when runtime service shutdown is required.
- Recalculate staleness in `sensor_manager_get_status()` if callers require
  wall-clock-accurate aging even when sampling stops.
- Add retry/backoff policy only if DHT22 failure behavior justifies it.
