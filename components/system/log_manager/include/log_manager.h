#pragma once

/** @file log_manager.h
 * Composition-root management interface. Lifecycle operations must be serialized
 * by one owner task. Producers may run concurrently, including during stop.
 * No API is ISR-safe. No API mounts SD, synchronizes time, or reboots the device.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log_level.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t produced_records;
    uint64_t persisted_records; /* fwrite accepted; not a durability guarantee */
    uint64_t dropped_records;
    uint32_t storage_write_failures;
    uint32_t storage_recoveries;
    uint32_t file_rotations;
    uint32_t truncated_records;
    uint32_t contention_drops;
    size_t buffered_bytes; /* ring payload plus writer's retry batch */
    size_t peak_buffered_bytes;
    size_t buffer_capacity; /* ring allocation, including record framing */
    bool storage_available;
    bool time_synchronized;
} log_manager_stats_t;

/** Allocate PSRAM ring/batch; idempotent. Call after PSRAM/RTOS startup.
 * Returns NO_MEM without internal-RAM fallback; NOT_SUPPORTED when disabled.
 * Static synchronization objects remain for application lifetime, even deinit.
 */
esp_err_t log_manager_init(void);
/** Create unpinned internal-stack writer; idempotent; requires init. */
esp_err_t log_manager_start(void);
/** Best effort sync of the current backlog, bounded caller wait in milliseconds.
 * Returns FAIL if storage unavailable/failing, TIMEOUT if still in progress.
 * A timed-out request may complete later. Does not cancel ongoing SD I/O.
 * An unsynced background close/close error is retained until a management
 * request reports FAIL, even if SD remounts before that request. A subsequent
 * request checks current work; it does not recover previously uncertain data.
 */
esp_err_t log_manager_flush(uint32_t timeout_ms);
/** Reject new persistence, drain once, sync/close, and stop writer.
 * On TIMEOUT keep resources alive; repeat stop before deinit/start.
 * FAIL means writer stopped with unavailable storage, failed cleanup or
 * uncertain durability. Unwritten backlog is retained for restart; batches
 * previously accepted by fwrite cannot be reconstructed after a sync failure.
 * Calling stop again on a stopped logger is idempotent.
 */
esp_err_t log_manager_stop(uint32_t timeout_ms);
/** Requires stopped writer. Frees PSRAM; buffered data is discarded and counted.
 * Idempotent. Static locks are deliberately retained, never dynamically leaked.
 */
esp_err_t log_manager_deinit(void);
/** Coherent bounded snapshot; INVALID_ARG for NULL, TIMEOUT on contention. */
esp_err_t log_manager_get_stats(log_manager_stats_t *stats);
void log_manager_set_console_enabled(bool enabled);
void log_manager_set_storage_enabled(bool enabled);
void log_manager_set_console_level(esp_log_level_t level);
void log_manager_set_storage_level(esp_log_level_t level);
/** Non-waiting lifecycle hint, called by composition SD/time callbacks.
 * No I/O, formatting, or allocation. A missed contended hint is covered by
 * the writer's bounded timeout and current provider snapshots.
 */
void log_manager_notify_environment_changed(void);
#ifdef __cplusplus
}
#endif
