/* Run the production writer on a pthread-backed RTOS shim and real host files.
 * Only device APIs and I/O fault injection are substituted. Shorter test policy
 * intervals/sizes keep failure/recovery scenarios deterministic and inexpensive.
 */
#include "platform.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
static int binary_open(const char *path, int flags, int mode)
{
    return open(path, flags | O_BINARY, mode);
}
#define open binary_open
#define fwrite host_fwrite
#define fsync host_fsync
#define mkdir host_mkdir
#define unlink host_unlink
#include "../../log_manager.c"
#undef fwrite
#undef fsync
#undef mkdir
#undef open
#undef unlink

static log_manager_stats_t snapshot(void)
{
    log_manager_stats_t result;
    assert(log_manager_get_stats(&result) == ESP_OK); return result;
}
static void mounted(bool ready)
{
    __atomic_store_n(&host_mounted, ready, __ATOMIC_RELAXED);
    log_manager_notify_environment_changed();
}
static void wait_persisted(uint64_t minimum)
{
    for (int i = 0; i < 500; ++i) {
        if (snapshot().persisted_records >= minimum) return;
        host_sleep(2);
    }
    fprintf(stderr, "persisted target=%llu actual=%llu\n", (unsigned long long)minimum,
            (unsigned long long)snapshot().persisted_records);
    assert(false);
}
static void wait_no_leases(void)
{
    for (int i = 0; i < 500 && __atomic_load_n(&host_leases, __ATOMIC_RELAXED); ++i) host_sleep(1);
    assert(__atomic_load_n(&host_leases, __ATOMIC_RELAXED) == 0);
}
static void read_text(const char *path, char *out, size_t capacity)
{
    FILE *file = fopen(path, "rb"); assert(file);
    size_t n = fread(out, 1, capacity - 1, file); out[n] = 0; fclose(file);
}
static void stop_ok(void)
{
    assert(log_manager_stop(2000) == ESP_OK);
    for (int i = 0; i < 500 && __atomic_load_n(&host_tasks, __ATOMIC_RELAXED); ++i) host_sleep(1);
    assert(host_tasks == 0 && host_leases == 0);
}
static void ring_tests(void)
{
    unsigned char memory[71]; char line[80]; log_buffer_t r;
    log_buffer_init(&r, memory, sizeof(memory));
    assert(log_buffer_push(&r, "first\n", 6) == 0);
    assert(log_buffer_push(&r, "second\n", 7) == 0);
    assert(log_buffer_peek(&r, line, sizeof(line)) == 6 && !strcmp(line, "first\n"));
    log_buffer_pop(&r);
    assert(log_buffer_peek(&r, line, sizeof(line)) == 7 && !strcmp(line, "second\n"));
    log_buffer_pop(&r);
    for (unsigned i = 0; i < 10000; ++i) {
        int n = snprintf(line, sizeof(line), "value=%06u abcdefghijklmnop\n", i);
        size_t dropped = log_buffer_push(&r, line, n);
        assert(dropped == (i >= 2 ? 1U : 0U));
        char head[80]; log_buffer_peek(&r, head, sizeof(head));
        unsigned value; assert(sscanf(head, "value=%u", &value) == 1);
        assert(value == (i ? i - 1 : 0));
        assert(r.used <= r.capacity && r.records <= 2);
    }
    char text[40] = "line1\nline2\r\033foo";
    size_t n = log_line_finish(text, sizeof(text), false);
    assert(n == strlen(text) && strchr(text, '\n') == text + n - 1 && !strchr(text, '\r'));
    memset(text, 'x', sizeof(text)); text[39] = 0;
    n = log_line_finish(text, sizeof(text), true);
    assert(n < sizeof(text) && strstr(text, "truncated=1\n"));
    puts("PASS ring FIFO, wraparound, DROP_OLDEST (10000 iterations), single-line/truncation");
}

static void *producer(void *arg)
{
    for (int i = 0; i < 500; ++i) APP_LOGD("TEST", CONCURRENT, "producer=%ld index=%d", (long)(intptr_t)arg, i);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    ring_tests();
    APP_LOGI("TEST", EARLY, "value=1"); assert(host_console == 1);
    assert(host_zero_boot_console == 0); /* early fallback has no structured boot field */
    log_manager_set_console_enabled(false);
    assert(log_manager_deinit() == ESP_OK);
    host_fail_alloc = 1;
    assert(log_manager_init() == ESP_ERR_NO_MEM && host_allocations == 0);
    assert(log_manager_init() == ESP_OK && log_manager_init() == ESP_OK && host_allocations == 1);
    host_fail_task = 1;
    assert(log_manager_start() == ESP_ERR_NO_MEM && host_tasks == 0);
    assert(log_manager_start() == ESP_OK && log_manager_start() == ESP_OK && host_tasks == 1);
    host_sleep(20);
    APP_LOGI("TEST", OFFLINE, "value=1");
    host_sleep(150);
    assert(snapshot().produced_records == 1 && snapshot().persisted_records == 0);
    assert(log_manager_flush(1000) == ESP_FAIL);
    mounted(true);
    wait_persisted(1);
    wait_no_leases();
    assert(log_manager_flush(1000) == ESP_OK);
    char unknown[PATH_BYTES]; strcpy(unknown, s_path);
    assert(strstr(unknown, "/unknown/boot_"));
    char content[8192]; read_text(unknown, content, sizeof(content));
    assert(strstr(content, "[UNSYNCED]") && strstr(content, "[OFFLINE]"));
    puts("PASS pre-init console, repeated init/start, SD absent startup, late SD, unsynced file");

    /* P3: once backlog is empty and data is synced, the writer blocks on task
     * notification rather than waking every WRITE_MS just to resnapshot providers. */
    int idle_waits = __atomic_load_n(&host_notify_waits, __ATOMIC_RELAXED);
    host_sleep(350);
    assert(__atomic_load_n(&host_notify_waits, __ATOMIC_RELAXED) <= idle_waits + 1);
    puts("PASS fully idle writer waits on notification without periodic polling");

    /* P0 regression: an idle log segment must not retain an SD lease. During
     * the SD manager's idle health check the logical state remains READY while
     * new leases are temporarily rejected; logger must buffer, not report loss.
     */
    uint64_t health_before = snapshot().persisted_records;
    __atomic_store_n(&host_health_check, 1, __ATOMIC_RELAXED);
    APP_LOGI("TEST", SD_HEALTH_CHECK_WINDOW, "value=1");
    log_manager_notify_environment_changed();
    host_sleep(150);
    log_manager_stats_t health = snapshot();
    assert(health.persisted_records == health_before);
    assert(health.buffered_bytes > 0 && health.storage_available);
    assert(__atomic_load_n(&host_leases, __ATOMIC_RELAXED) == 0);
    __atomic_store_n(&host_health_check, 0, __ATOMIC_RELAXED);
    log_manager_notify_environment_changed();
    wait_persisted(health_before + 1);
    wait_no_leases();
    assert(log_manager_flush(1000) == ESP_OK);
    wait_no_leases();
    puts("PASS idle logger releases SD lease and tolerates SD health-check reservation");

    uint64_t before = snapshot().persisted_records;
    APP_LOGI("TEST", TIMEOUT, "value=2");
    wait_persisted(before + 1);
    wait_no_leases();
    int sync_before = __atomic_load_n(&host_syncs, __ATOMIC_RELAXED);
    host_sleep(320);
    assert(__atomic_load_n(&host_syncs, __ATOMIC_RELAXED) > sync_before);
    wait_no_leases();
    puts("PASS first-record wake, timeout drain and independent durability sync");

    assert(log_manager_flush(1000) == ESP_OK);
    int writes_before = __atomic_load_n(&host_writes, __ATOMIC_RELAXED);
    char payload[240]; memset(payload, 't', sizeof(payload) - 1); payload[239] = 0;
    for (int i = 0; i < 5; ++i) APP_LOGI("TEST", THRESHOLD, "index=%d text=%s", i, payload);
    for (int i = 0; i < 40 && __atomic_load_n(&host_writes, __ATOMIC_RELAXED) == writes_before; ++i) host_sleep(2);
    assert(__atomic_load_n(&host_writes, __ATOMIC_RELAXED) > writes_before);
    wait_no_leases();
    assert(log_manager_flush(1000) == ESP_OK);
    before = snapshot().persisted_records - 1;
    puts("PASS threshold drain before write timeout");

    __atomic_store_n(&host_synced, 1, __ATOMIC_RELAXED);
    log_manager_notify_environment_changed();
    wait_persisted(before + 2);
    assert(log_manager_flush(1000) == ESP_OK);
    assert(!strstr(s_path, "/unknown/"));
    read_text(unknown, content, sizeof(content));
    assert(strstr(content, "[TIME_SYNC]"));
    read_text(s_path, content, sizeof(content));
    assert(strstr(content, "[TIME_SYNC]") && !strstr(content, "UNSYNCED"));
    assert(snapshot().time_synchronized);
    puts("PASS late time sync, marker, dated rollover, unchanged unknown path");

    log_manager_set_storage_enabled(false);
    uint64_t produced = snapshot().produced_records;
    log_manager_set_console_enabled(true);
    log_manager_set_console_level(ESP_LOG_WARN);
    int console_before = __atomic_load_n(&host_console, __ATOMIC_RELAXED);
    APP_LOGI("TEST", FILTERED, "value=3");
    APP_LOGW("TEST", CONSOLE_ONLY, "value=4");
    assert(snapshot().produced_records == produced);
    assert(__atomic_load_n(&host_console, __ATOMIC_RELAXED) == console_before + 1);
    log_manager_set_console_level(ESP_LOG_VERBOSE);
    APP_LOGV("TEST", CONSOLE_VERBOSE, "value=5");
    assert(__atomic_load_n(&host_console, __ATOMIC_RELAXED) == console_before + 2);
    log_manager_set_console_level(ESP_LOG_WARN);
    log_manager_set_storage_enabled(true);
    log_manager_set_storage_level(ESP_LOG_DEBUG);
    APP_LOGD("TEST", STORAGE_DEBUG, "value=5");
    assert(snapshot().produced_records == produced + 1);
    APP_LOGV("TEST", FILTERED_VERBOSE, "value=6");
    assert(snapshot().produced_records == produced + 1);
    log_manager_set_console_enabled(false);
    assert(log_manager_flush(1000) == ESP_OK);
    puts("PASS independent console/storage switches and levels, persistent DEBUG");

    char large[1000]; memset(large, 'x', sizeof(large) - 1); large[999] = 0;
    APP_LOGI("TEST", TRUNCATED, "text=%s\nextra=1", large);
    assert(log_manager_flush(1000) == ESP_OK);
    assert(snapshot().truncated_records == 1);
    read_text(s_path, content, sizeof(content));
    assert(strstr(content, "truncated=1\n"));
    puts("PASS bounded max record and truncation counter");

    __atomic_store_n(&host_fail_write, 1, __ATOMIC_RELAXED);
    APP_LOGE("TEST", WRITE_FAIL, "fault=injected");
    for (int i = 0; i < 500 && !snapshot().storage_write_failures; ++i) host_sleep(2);
    assert(snapshot().storage_write_failures == 1 && snapshot().buffered_bytes > 0);
    produced = snapshot().produced_records;
    host_sleep(150);
    assert(snapshot().produced_records == produced); /* no recursive logging */
    mounted(true); wait_persisted(produced - snapshot().dropped_records);
    assert(snapshot().storage_recoveries == 1);
    assert(log_manager_flush(1000) == ESP_OK);
    puts("PASS write fault, no recursion, retained retry batch and recovery");

    __atomic_store_n(&host_fail_sync, 1, __ATOMIC_RELAXED);
    APP_LOGI("TEST", SYNC_FAIL, "fault=injected");
    assert(log_manager_flush(1000) == ESP_FAIL);
    assert(snapshot().storage_write_failures == 2 && host_leases == 0);
    mounted(true);
    APP_LOGI("TEST", SYNC_RECOVER, "value=1");
    assert(log_manager_flush(1000) == ESP_OK);
    puts("PASS durability error closes/releases storage safely");

    stop_ok();
    mounted(false);
    assert(log_manager_start() == ESP_OK); host_sleep(20);
    for (int i = 0; i < 200; ++i) APP_LOGI("TEST", OVERFLOW, "index=%d payload=abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", i);
    assert(snapshot().dropped_records > 0 && snapshot().buffered_bytes <= 16384 + 1024);
    mounted(true);
    assert(log_manager_flush(2000) == ESP_OK);
    assert(snapshot().file_rotations > 0);
    puts("PASS unavailable overflow, bounded memory, rotation and backlog drain");

    pthread_t producers[4];
    for (intptr_t i = 0; i < 4; ++i) assert(!pthread_create(&producers[i], NULL, producer, (void *)i));
    for (int i = 0; i < 4; ++i) pthread_join(producers[i], NULL);
    assert(log_manager_flush(2000) == ESP_OK);
    log_manager_stats_t concurrent = snapshot();
    assert(concurrent.produced_records == concurrent.persisted_records + concurrent.dropped_records);
    assert(concurrent.buffered_bytes == 0);
    uint32_t contention = concurrent.contention_drops;
    int zero_boot = __atomic_load_n(&host_zero_boot_console, __ATOMIC_RELAXED);
    log_manager_set_console_enabled(true);
    log_manager_set_console_level(ESP_LOG_INFO);
    assert(lock(20));
    APP_LOGI("TEST", CONTENDED, "value=1");
    unlock();
    log_manager_set_console_enabled(false);
    assert(snapshot().contention_drops == contention + 1);
    assert(__atomic_load_n(&host_zero_boot_console, __ATOMIC_RELAXED) == zero_boot);
    puts("PASS concurrent producers, contention drop and stable boot identity on console fallback");

    /* Force a slow SD write: producer remains quick, stop times out without
     * deleting its task/buffer, then a repeated stop reaps the same operation.
     */
    __atomic_store_n(&host_delay, 250, __ATOMIC_RELAXED);
    int64_t start = esp_timer_get_time();
    APP_LOGE("TEST", SLOW_CARD, "value=1");
    assert(esp_timer_get_time() - start < 100000);
    assert(log_manager_stop(5) == ESP_ERR_TIMEOUT);
    assert(log_manager_deinit() == ESP_ERR_INVALID_STATE);
    assert(log_manager_stop(2000) == ESP_OK);
    __atomic_store_n(&host_delay, 0, __ATOMIC_RELAXED);
    for (int i = 0; i < 500 && host_tasks; ++i) host_sleep(1);
    assert(host_tasks == 0 && host_leases == 0);
    assert(log_manager_stop(100) == ESP_OK);
    puts("PASS async ERROR, bounded stop timeout/retry and deinit guard");

    /* Retention acts only on exact owned names and never the active path. */
    host_mkdir("sd/logs/2000_01_01", 0775);
    const char *old = "sd/logs/2000_01_01/00_00_00_boot_0000000000000000_000.log";
    FILE *file = fopen(old, "wb"); assert(file);
    assert(fseek(file, 1024 * 1024, SEEK_SET) == 0); fputc('x', file); fclose(file);
    const char *unowned = "sd/logs/2000_01_01/important.txt";
    file = fopen(unowned, "w"); assert(file); fputs("keep", file); fclose(file);
    host_fail_unlink = 1;
    assert(log_manager_start() == ESP_OK);
    APP_LOGI("TEST", RETENTION, "value=1");
    assert(log_manager_flush(2000) == ESP_FAIL);
    assert(access(old, 0) == 0 && access(unowned, 0) == 0);
    log_manager_notify_environment_changed();
    assert(log_manager_flush(2000) == ESP_OK);
    assert(access(old, 0) != 0 && access(unowned, 0) == 0 && access(s_path, 0) == 0);
    stop_ok();
    assert(log_manager_deinit() == ESP_OK && log_manager_deinit() == ESP_OK && host_allocations == 0);
    assert(log_manager_init() == ESP_OK && host_allocations == 1);
    assert(log_manager_start() == ESP_OK); stop_ok();
    assert(log_manager_deinit() == ESP_OK && host_allocations == 0);
    puts("PASS retention, active/unowned file protection, restart/deinit, no task/lease/buffer leaks");
    puts("ALL HOST LOG MANAGER TESTS PASSED");
    return 0;
}