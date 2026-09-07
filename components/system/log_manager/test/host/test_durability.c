/* Regression for write-before-sync followed by unavailable storage/close error.
 * Separate processes keep counters and lifecycle fixtures independent. */
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_close;
static int binary_open(const char *path, int flags, int mode)
{
    return open(path, flags | O_BINARY, mode);
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

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    bool stop = strstr(argv[1], "stop") != NULL;
    bool close_error = strstr(argv[1], "close") != NULL;
    bool background = strstr(argv[1], "background") != NULL;
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
    if (close_error) __atomic_store_n(&fail_close, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&host_mounted, 0, __ATOMIC_RELAXED);
    unlock();
    if (!dirty || syncs) return 5;
    if (background) {
        log_manager_notify_environment_changed();
        for (int i = 0; i < 1000 && __atomic_load_n(&host_leases, __ATOMIC_RELAXED); ++i) host_sleep(1);
        if (__atomic_load_n(&host_leases, __ATOMIC_RELAXED)) return 6;
        __atomic_store_n(&host_mounted, 1, __ATOMIC_RELAXED);
        log_manager_notify_environment_changed();
    }
    esp_err_t result = stop ? log_manager_stop(1000) : log_manager_flush(1000);
    if (result != ESP_FAIL) {
        fprintf(stderr, "FAIL %s: result=%d expected=%d\n", argv[1], result, ESP_FAIL);
        return 7;
    }
    if (log_manager_get_stats(&stats) != ESP_OK || stats.buffered_bytes) return 8;
    if (close_error && stats.storage_write_failures != 1) return 9;
    if (!stop) {
        __atomic_store_n(&host_mounted, 1, __ATOMIC_RELAXED);
        log_manager_notify_environment_changed();
        /* The prior failed outcome was delivered; a new request can succeed. */
        if (log_manager_flush(1000) != ESP_OK || log_manager_stop(1000) != ESP_OK) return 10;
    }
    for (int i = 0; i < 1000 && __atomic_load_n(&host_tasks, __ATOMIC_RELAXED); ++i) host_sleep(1);
    if (host_tasks || host_leases || log_manager_deinit() != ESP_OK || host_allocations) return 11;
    printf("PASS durability %s: ESP_FAIL propagated, cleanup complete\n", argv[1]);
    return 0;
}
