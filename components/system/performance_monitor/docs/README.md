# performance_monitor Component Notes

## Purpose

`performance_monitor` starts a low-priority diagnostic task that periodically
logs CPU utilization, heap health, application partition usage, and its own
stack high-water mark.

This component reports diagnostics to the serial log and copies its latest
completed CPU sample through a small public read-only status API. It does not
create LVGL performance widgets, take measurements on behalf of callers, or
expose task handles, addresses, raw snapshots, or heap dumps.

## What Is Done

- Measures five-second CPU busy and idle percentages from FreeRTOS runtime
  snapshots.
- Records `peak_500ms`, the highest CPU busy percentage from ten nominal
  500 ms samples within each five-second report cycle.
- Accounts for the configured number of CPU cores when calculating total CPU
  capacity.
- Logs internal 8-bit RAM, PSRAM, and internal DMA-capable RAM.
- Logs total, used, current free, minimum free, and largest free block values.
- Logs running application image size and partition capacity immediately and
  every 12 samples.
- Logs the monitor task's minimum remaining stack.
- Uses a 5-second measurement period, 6 KB task stack, and priority 2.
- Prevents a second monitor task from being started.
- Defers a report cycle when `VOICE_RECORDING_CRITICAL` is active, including a
  cycle whose 500-ms CPU sample detects capture partway through. The monitor
  waits for the lightweight transition notification and resumes normally after
  capture; it is not stopped, deleted, or recreated per turn.
- Copies only the most recent completed CPU sample (average, 500-ms peak, idle,
  report index, and monotonic capture time) under a short critical section.

## Public API

```c
esp_err_t performance_monitor_start(void);
esp_err_t performance_monitor_get_status(performance_monitor_status_t *status);
```

Example:

```c
esp_err_t ret = performance_monitor_start();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Performance monitor failed: %s", esp_err_to_name(ret));
}
```

`performance_monitor_get_status()` is task-context, non-blocking, allocation-
free, and never starts a sample or writes a log. It returns
`ESP_ERR_INVALID_STATE` until the monitor has started; `sample_valid=false`
means startup or a deferred/failed measurement has not produced a completed
sample yet. Callers must treat the capture time as snapshot freshness, not as a
request-time metric.

## Configuration Requirements

CPU measurement requires:

```text
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
```

`CONFIG_PERFORMANCE_MONITOR_ENABLE` controls whether `smart_room_app` starts
the monitor during product boot. It defaults to `y`; disable it through
`menuconfig` when periodic diagnostic logging is not wanted.

When runtime statistics are disabled, `performance_monitor_start()` returns
`ESP_ERR_NOT_SUPPORTED` and does not create a task.

## Expected Logs

The task emits records similar to:

```text
I (...) PERF_MONITOR: [CPU] used=12.3%, peak_500ms=28.6%, idle=87.7%, cores=2
I (...) PERF_MONITOR: [RAM:INTERNAL] total=..., used=..., free=..., minimum=..., largest=... bytes
I (...) PERF_MONITOR: [RAM:PSRAM] total=..., used=..., free=..., minimum=..., largest=... bytes
I (...) PERF_MONITOR: [RAM:DMA] total=..., used=..., free=..., minimum=..., largest=... bytes
I (...) PERF_MONITOR: [STACK] task=perf_monitor, minimum remaining=... bytes
```

Each high-level report block (header, CPU, RAM, task summary, and the
occasional task table) has a console-only blank line before it; one final blank
line closes the report. The spacing is not persisted as an empty log record.

## Important Notes

- `used` is system-wide runtime utilization over the nominal five-second
  window. `peak_500ms` is the maximum of ten nominal 500 ms system-wide
  samples from that same cycle; it is not an instantaneous hardware peak.
  Both are different from LVGL's optional performance-overlay CPU estimate.
- CPU accuracy depends on FreeRTOS runtime statistics and its timer source.
- The extra peak measurement captures ten end snapshots instead of one per
  report cycle. It remains in the low-priority monitor task, but has more
  diagnostic overhead than average-only measurement.
- The monitor supports up to 40 tasks in one snapshot. If the system has more,
  CPU measurement returns `ESP_ERR_INVALID_SIZE` and logs an error.
- `minimum` heap means the lowest free heap observed since boot. `largest`
  helps diagnose fragmentation and contiguous allocation failures.
- Heap capability regions overlap. Do not add INTERNAL, PSRAM, and DMA totals
  together as if they were independent memory pools.
- Calling `performance_monitor_start()` again returns
  `ESP_ERR_INVALID_STATE`.
- The recording-window policy avoids task snapshots, task-table output, heap
  reports, and their console burst while capture is active. It is a diagnostic
  deferral only and does not change audio, network, or scheduler ownership.

## Future Attention

- Add a stop API only if runtime enable/disable is required.
- Add further copied fields only after an owner-specific public contract and
  presentation need are approved; do not scrape console logs for diagnostics.
- Raise `PERF_MONITOR_MAX_TASKS` if the application grows beyond 40 tasks.
