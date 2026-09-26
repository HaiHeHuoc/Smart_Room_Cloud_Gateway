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
    uint64_t persisted_records; /* complete records accepted by fwrite; not a durability guarantee */
    uint64_t durable_records; /* subset confirmed by successful fsync */
    uint64_t durability_uncertain_records; /* fwrite-accepted records lost behind an uncertain/failed sync boundary */
    uint64_t dropped_records;
    uint32_t storage_write_failures;
    uint32_t storage_recoveries;
    uint32_t durability_syncs;
    uint32_t durability_sync_failures;
    uint32_t partial_write_events;
    uint32_t writer_start_failures;
    uint32_t file_rotations;
    uint32_t truncated_records;
    uint32_t contention_drops;
    /** Writer safe-point deferrals while live microphone capture was active. */
    uint32_t recording_critical_deferred_cycles;
    size_t buffered_bytes; /* ring payload plus writer's retry batch */
    size_t peak_buffered_bytes;
    size_t buffer_capacity; /* ring allocation, including record framing */
    bool storage_available;
    bool time_synchronized;
} log_manager_stats_t;

#define LOG_MANAGER_ARCHIVE_ID_LEN 16U
#define LOG_MANAGER_ARCHIVE_LIST_MAX 12U
#define LOG_MANAGER_ARCHIVE_PAGE_MAX_RECORDS 6U
#define LOG_MANAGER_ARCHIVE_PAGE_MAX_SCAN_BYTES 4096U
#define LOG_MANAGER_PUBLIC_TIMESTAMP_MAX_LEN 23U
#define LOG_MANAGER_PUBLIC_TAG_MAX_LEN 24U
#define LOG_MANAGER_PUBLIC_EVENT_MAX_LEN 40U

typedef struct {
    char id[LOG_MANAGER_ARCHIVE_ID_LEN + 1U];
    uint64_t size_bytes;
    bool time_named;
    /** Internal ordering metadata; Local Web deliberately does not serialize it. */
    int64_t sort_time;
} log_manager_archive_t;

typedef struct {
    bool available;
    bool truncated;
    uint8_t count;
    log_manager_archive_t archives[LOG_MANAGER_ARCHIVE_LIST_MAX];
} log_manager_archive_list_t;

typedef struct {
    bool time_valid;
    char timestamp[LOG_MANAGER_PUBLIC_TIMESTAMP_MAX_LEN + 1U];
    uint64_t uptime_ms;
    char level[8U];
    char tag[LOG_MANAGER_PUBLIC_TAG_MAX_LEN + 1U];
    char event[LOG_MANAGER_PUBLIC_EVENT_MAX_LEN + 1U];
} log_manager_public_record_t;

typedef struct {
    bool available;
    bool eof;
    /** False when a scan budget ended inside one malformed line. */
    bool next_offset_valid;
    bool details_omitted;
    uint64_t offset;
    uint64_t next_offset;
    uint16_t malformed_record_count;
    uint8_t record_count;
    log_manager_public_record_t records[LOG_MANAGER_ARCHIVE_PAGE_MAX_RECORDS];
} log_manager_archive_page_t;

/** Allocate PSRAM ring/batch; idempotent. Call after PSRAM/RTOS startup.
 * Returns NO_MEM without internal-RAM fallback; NOT_SUPPORTED when disabled.
 * Static synchronization objects remain for application lifetime, even deinit.
 */
esp_err_t log_manager_init(void);
/** Create unpinned internal-stack writer; idempotent; requires init.
 * If task creation fails, persistence enters a fail-closed degraded state:
 * console logging continues, but new persistent records are not enqueued until
 * a later successful start. Any backlog captured before the failure is retained.
 */
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
/** Coherent bounded snapshot; INVALID_ARG for NULL, TIMEOUT on contention.
 * persisted_records counts complete records accepted by fwrite. durable_records
 * advances only after fsync succeeds. durability_uncertain_records counts
 * previously accepted records whose durability later became uncertain.
 */
esp_err_t log_manager_get_stats(log_manager_stats_t *stats);
/** Copy closed, manager-owned archive metadata. IDs are opaque lookup keys. */
esp_err_t log_manager_list_archives(log_manager_archive_list_t *archives);
/**
 * Copy one bounded page of sanitized structured records from a closed archive.
 * Raw free-form details are never copied. @p offset must be zero or a line
 * boundary previously returned as next_offset.
 */
esp_err_t log_manager_read_archive(const char *archive_id, uint64_t offset,
                                   log_manager_archive_page_t *page);
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
