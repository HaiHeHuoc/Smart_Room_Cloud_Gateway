# performance_monitor Component Notes

## Purpose

`performance_monitor` starts a low-priority diagnostic task that periodically
logs CPU utilization, heap health, application partition usage, and its own
stack high-water mark.

This component reports diagnostics to the serial log. It does not create LVGL
performance widgets or expose sampled values to application code.

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

## Public API

```c
esp_err_t performance_monitor_start(void);
```

Example:

```c
esp_err_t ret = performance_monitor_start();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Performance monitor failed: %s", esp_err_to_name(ret));
}
```

## Configuration Requirements

CPU measurement requires:

```text
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
```

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

## Future Attention

- Add a stop API only if runtime enable/disable is required.
- Expose structured samples through a callback or queue only when another
  component needs the values.
- Raise `PERF_MONITOR_MAX_TASKS` if the application grows beyond 40 tasks.
