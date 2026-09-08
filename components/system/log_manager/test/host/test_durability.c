/* Regression for write-before-sync followed by unavailable storage/close error,
 * writer-start degradation, and partial fwrite recovery. Separate processes keep
 * counters and lifecycle fixtures independent. */
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_close;
static int binary_open(const char *path, int flags, ...)
{
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode_t mode = (mode_t)va_arg(args, int);
        va_end(args);
        return open(path, flags | O_BINARY, mode);
    }
    return open(path, flags | O_BINARY);
}
static int injected_close(FILE *file)
{
    int result = fclose(file);
    if (__atomic_exchange_n(&fail_close, 0, __ATOMIC_RELAXED)) {
        errno = EIO;
        return EOF;
    }
    return result;
}
#define fclose injected_close
#define open binary_open
#define fwrite host_fwrite
#define fsync host_fsync
#define mkdir host_mkdir
#include "../../log_manager.c"

static int cleanup(void)
{
    for (int i = 0; i < 1000 && __atomic_load_n(&host_tasks, __ATOMIC_RELAXED); ++i) host_sleep(1);
    return host_tasks || host_leases || log_manager_deinit() != ESP_OK || host_allocations;
}

static int test_start_fail(void)
{
    log_manager_set_console_enabled(false);
    host_mounted = 1;
    if (log_manager_init() != ESP_OK) return 20;
    host_fail_task = 1;
    if (log_manager_start() != ESP_ERR_NO_MEM) return 21;
    log_manager_stats_t before = {0}, after = {0};
    if (log_manager_get_stats(&before) != ESP_OK || before.writer_start_failures != 1 || before.storage_available) return 22;
    APP_LOGI("TEST", WRITER_START_FAILED, "must_not_queue=1");
    if (log_manager_get_stats(&after) != ESP_OK || after.produced_records != before.produced_records || after.buffered_bytes) return 23;
    if (log_manager_start() != ESP_OK) return 24;
    APP_LOGI("TEST", WRITER_RECOVERED, "queue=1");
    if (log_manager_flush(1000) != ESP_OK) return 25;
    if (log_manager_get_stats(&after) != ESP_OK || after.produced_records != before.produced_records + 1 ||
        after.persisted_records < 1 || after.durable_records < 1) return 26;
    if (log_manager_stop(1000) != ESP_OK || cleanup()) return 27;
    puts("PASS writer start failure is persistence fail-closed until successful retry");
    return 0;
}

static int test_partial_write(void)
{
    log_manager_set_console_enabled(false);
    host_mounted = 1;
    if (log_manager_init() != ESP_OK || log_manager_start() != ESP_OK) return 30;
    log_manager_stats_t before = {0}, failed = {0}, after = {0};
    if (log_manager_get_stats(&before) != ESP_OK) return 31;
    host_partial_write = 1;
    const char *payload = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    APP_LOGI("TEST", PARTIAL_WRITE, "index=0 payload=%s", payload);
    APP_LOGI("TEST", PARTIAL_WRITE, "index=1 payload=%s", payload);
    APP_LOGI("TEST", PARTIAL_WRITE, "index=2 payload=%s", payload);
    if (log_manager_flush(1000) != ESP_FAIL) return 32;
    if (log_manager_get_stats(&failed) != ESP_OK || failed.partial_write_events != before.partial_write_events + 1 ||
        failed.storage_write_failures != before.storage_write_failures + 1 ||
        failed.durability_uncertain_records <= before.durability_uncertain_records ||
        failed.dropped_records <= before.dropped_records || !failed.buffered_bytes) return 33;
    host_mounted = 1;
    log_manager_notify_environment_changed();
    if (log_manager_flush(2000) != ESP_OK) return 34;
    if (log_manager_get_stats(&after) != ESP_OK) return 35;
    uint64_t produced_delta = after.produced_records - before.produced_records;
    uint64_t persisted_delta = after.persisted_records - before.persisted_records;
    uint64_t dropped_delta = after.dropped_records - before.dropped_records;
    uint64_t durable_delta = after.durable_records - before.durable_records;
    uint64_t uncertain_delta = after.durability_uncertain_records - before.durability_uncertain_records;
    if (produced_delta != 3 || persisted_delta + dropped_delta != produced_delta ||
        durable_delta + uncertain_delta != persisted_delta || after.buffered_bytes ||
        after.partial_write_events != before.partial_write_events + 1) return 36;
    if (log_manager_stop(1000) != ESP_OK || cleanup()) return 37;
    puts("PASS partial fwrite consumes accepted prefix once, drops split record, retries only clean records");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    if (!strcmp(argv[1], "start_fail")) return test_start_fail();
    if (!strcmp(argv[1], "partial_write")) return test_partial_write();

    bool stop = strstr(argv[1], "stop") != NULL;
    bool close_error = strstr(argv[1], "close") != NULL;
    bool background = strstr(argv[1], "background") != NULL;
    bool inject_close_error = close_error && !background;
    log_manager_set_console_enabled(false);
    host_mounted = 1;
    if (log_manager_init() != ESP_OK) return 2;
    APP_LOGI("TEST", DURABILITY, "record=1");
    if (log_manager_start() != ESP_OK) return 3;
    log_manager_stats_t stats = {0};
    for (int i = 0; i < 1000; ++i) {
        if (log_manager_get_stats(&stats) == ESP_OK && stats.persisted_records) break;
        host_sleep(1);
    }
    if (!stats.persisted_records || !lock(20)) return 4;
    bool dirty = s_dirty;
    int syncs = __atomic_load_n(&host_syncs, __ATOMIC_RELAXED);
    if (inject_close_error) __atomic_store_n(&fail_close, 1, __ATOMIC_RELAXED);
    /* The writer parks idle files to release the SD lease. Exercise a close
     * failure during the next reopen/sync, while ordinary cases still model
     * media becoming unavailable before the dirty segment can be synced. */
    if (!close_error || background) __atomic_store_n(&host_mounted, 0, __ATOMIC_RELAXED);
    unlock();
    if (!dirty || syncs) return 5;
    if (background) {
        log_manager_notify_environment_changed();
        bool offline_observed = false;
        for (int i = 0; i < 1000; ++i) {
            if (log_manager_get_stats(&stats) == ESP_OK && !stats.storage_available) {
                offline_observed = true;
                break;
            }
            host_sleep(1);
        }
        if (!offline_observed || __atomic_load_n(&host_leases, __ATOMIC_RELAXED)) return 6;
        __atomic_store_n(&host_mounted, 1, __ATOMIC_RELAXED);
        log_manager_notify_environment_changed();
    }
    esp_err_t result = stop ? log_manager_stop(1000) : log_manager_flush(1000);
    if (result != ESP_FAIL) {
        fprintf(stderr, "FAIL %s: result=%d expected=%d\n", argv[1], result, ESP_FAIL);
        return 7;
    }
    if (log_manager_get_stats(&stats) != ESP_OK || stats.buffered_bytes) return 8;
    if (inject_close_error) {
        if (stats.storage_write_failures != 1 || !stats.durable_records ||
            stats.durability_uncertain_records) return 9;
    } else if (!stats.durability_uncertain_records || stats.durable_records) {
        return 9;
    }
    if (!stop) {
        __atomic_store_n(&host_mounted, 1, __ATOMIC_RELAXED);
        log_manager_notify_environment_changed();
        /* The prior failed outcome was delivered; a new request can succeed. */
        if (log_manager_flush(1000) != ESP_OK || log_manager_stop(1000) != ESP_OK) return 11;
    }
    if (cleanup()) return 12;
    printf("PASS durability %s: ESP_FAIL propagated, durability accounting and cleanup complete\n", argv[1]);
    return 0;
}
