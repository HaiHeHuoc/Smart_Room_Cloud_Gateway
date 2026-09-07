/* Includes ----------------------------------------------------------------- */
#include "log_manager.h"
#include "app_log.h"
#include "app_log_backend.h"
#include "log_buffer.h"
#include "board_config.h"
#include "sd_card_manager.h"
#include "time_manager.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* Macros ------------------------------------------------------------------- */
#define RECORD_BYTES CONFIG_LOG_MANAGER_RECORD_BYTES
#define BATCH_BYTES CONFIG_LOG_MANAGER_WRITE_BYTES
#define ROOT SD_MOUNT_POINT "/logs"
#define PATH_BYTES 192
#define WAKE_DATA 1U
#define WAKE_ENV 2U
#define WAKE_URGENT 4U
#define WAKE_REQUEST 8U
#define LOCK_TICKS pdMS_TO_TICKS(20)
#define WRITER_STACK_BYTES 6144
#define CONSOLE_SINK_TAG "APP_LOG_SINK"

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "LOG_MANAGER";

/* Type Definitions --------------------------------------------------------- */
typedef struct {
    log_buffer_t ring;
    char *batch;
    size_t pending_bytes, pending_records;
    log_manager_stats_t stats;
    TaskHandle_t task;
    bool initialized, running, stopping;
    uint32_t request, completed;
    esp_err_t request_result;
    uint64_t boot;
} manager_t;

/* Static Variables --------------------------------------------------------- */
/* INTERNAL_REQUIRED: locks, state and task stack/TCB. Never place in PSRAM. */
#if CONFIG_LOG_MANAGER_ENABLE
static StaticSemaphore_t s_mutex_memory, s_done_memory;
#endif
static SemaphoreHandle_t s_mutex, s_done;
static manager_t s;
static uint32_t s_contention;
static bool s_console = true, s_storage = true;
static int s_console_level = CONFIG_LOG_MANAGER_CONSOLE_LEVEL;
static int s_storage_level = CONFIG_LOG_MANAGER_STORAGE_LEVEL;
/* Writer-only file state; every filesystem call below runs in this context. */
static FILE *s_file;
static bool s_lease, s_dirty, s_failed;
/* Preserve an uncertain durability outcome across background close/remount
 * until a management request has observed it. An empty ring is not a sync. */
static bool s_durability_failed;
static char s_path[PATH_BYTES], s_day[11];
static size_t s_file_bytes;
static uint32_t s_sequence;
static int64_t s_last_sync, s_retry_after;
static char s_dated_prefix[25];

/* Static Functions --------------------------------------------------------- */
static bool lock(TickType_t ticks)
{
    SemaphoreHandle_t mutex = __atomic_load_n(&s_mutex, __ATOMIC_ACQUIRE);
    return mutex && xSemaphoreTake(mutex, ticks) == pdTRUE;
}

static void unlock(void) { xSemaphoreGive(s_mutex); }

static void wake_locked(uint32_t bits)
{
    if (s.task) xTaskNotify(s.task, bits, eSetBits);
}

static bool policy(bool *enabled, int *maximum, esp_log_level_t level)
{
    return level > ESP_LOG_NONE && level <= ESP_LOG_VERBOSE &&
           __atomic_load_n(enabled, __ATOMIC_RELAXED) &&
           (int)level <= __atomic_load_n(maximum, __ATOMIC_RELAXED);
}

static size_t format_line(char *line, esp_log_level_t level, const char *tag,
                          const char *event, const char *format, va_list args,
                          uint64_t boot, bool *truncated)
{
    char timestamp[32] = "UNSYNCED";
    struct timeval tv;
    struct tm local;
    if (time_manager_is_synced() && gettimeofday(&tv, NULL) == 0) {
        time_t seconds = tv.tv_sec;
        if (localtime_r(&seconds, &local)) {
            size_t n = strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
            if (n) snprintf(timestamp + n, sizeof(timestamp) - n, ".%03d", (int)(tv.tv_usec / 1000));
        }
    }
    int prefix = snprintf(line, RECORD_BYTES - 1,
        "[%s][+%06" PRIi64 "ms][%c][%.24s][%.40s][boot=%016" PRIx64 "] ",
        timestamp, esp_timer_get_time() / 1000, "NEWIDV"[level], tag, event, boot);
    size_t used = prefix < 0 ? 0 : (size_t)prefix;
    if (used >= RECORD_BYTES - 1) used = RECORD_BYTES - 2;
    int details = vsnprintf(line + used, RECORD_BYTES - 1 - used, format, args);
    *truncated = prefix < 0 || details < 0 ||
                 (size_t)details >= RECORD_BYTES - 1 - used ||
                 strnlen(tag, 25) > 24 || strnlen(event, 41) > 40;
    return log_line_finish(line, RECORD_BYTES, *truncated);
}

static void count_failure(int error)
{
    if (!error) error = EIO;
    if (lock(LOCK_TICKS)) {
        ++s.stats.storage_write_failures;
        s.stats.storage_available = false;
        unlock();
    }
    /* Never APP_LOG here: storage diagnostics must not feed persistence. */
    ESP_LOGW(TAG, "Storage operation failed errno=%d; preserving retry batch", error);
    s_failed = true;
    s_retry_after = esp_timer_get_time() + 5000000;
    if (sd_card_manager_is_vfs_media_error(error))
        sd_card_manager_report_io_error(ESP_FAIL);
}

static bool sync_file(void)
{
    if (!s_file || !s_dirty) return true;
    if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
        count_failure(errno);
        return false;
    }
    s_dirty = false;
    s_last_sync = esp_timer_get_time();
    return true;
}

static bool close_file(bool sync)
{
    if (!sync && s_dirty) s_durability_failed = true;
    bool ok = !sync || sync_file();
    if (s_file) {
        if (fclose(s_file) != 0) { count_failure(errno); ok = false; }
        s_file = NULL;
    }
    if (s_lease) { sd_card_manager_release(); s_lease = false; }
    s_path[0] = 0;
    s_dirty = false;
    if (!ok) s_durability_failed = true;
    return ok;
}

static bool join_path(char *out, const char *parent, const char *name)
{
    size_t a = strlen(parent), b = strlen(name);
    if (a + b + 2 > PATH_BYTES) return false;
    memcpy(out, parent, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
    return true;
}

static bool owned_directory(const char *name)
{
    if (!strcmp(name, "unknown")) return true;
    if (strlen(name) != 10) return false;
    for (size_t i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) { if (name[i] != '_') return false; }
        else if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static bool owned_file(const char *name)
{
    /* Only this component's exact filename grammar is eligible for deletion. */
    const char *p = name;
    if (!strncmp(p, "boot_", 5)) p += 5;
    else {
        if (strlen(p) < 14) return false;
        for (int i = 0; i < 8; ++i) {
            if (i == 2 || i == 5) { if (p[i] != '_') return false; }
            else if (p[i] < '0' || p[i] > '9') return false;
        }
        if (strncmp(p + 8, "_boot_", 6)) return false;
        p += 14;
    }
    if (strlen(p) < 22) return false;
    for (int i = 0; i < 16; ++i)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
    p += 16;
    if (*p++ != '_') return false;
    const char *digits = p;
    while (*p >= '0' && *p <= '9') ++p;
    return p > digits && !strcmp(p, ".log");
}

/* Constant-memory, two-level scan. Metadata operations are writer-only and
 * hold the active file's lease. Re-scan after each deletion: simple V1 policy.
 * A scan/delete error halts this pass instead of risking unrelated files.
 */
static bool retain(size_t incoming)
{
    for (;;) {
        uint64_t total = incoming;
        char oldest[PATH_BYTES] = "";
        time_t oldest_time = 0;
        bool ok = true;
        DIR *root = opendir(ROOT);
        if (!root) { count_failure(errno); return false; }
        struct dirent *entry;
        errno = 0;
        while ((entry = readdir(root))) {
            if (!owned_directory(entry->d_name)) continue;
            char directory[PATH_BYTES];
            if (!join_path(directory, ROOT, entry->d_name)) continue;
            DIR *dir = opendir(directory);
            if (!dir) { ok = false; break; }
            struct dirent *file;
            errno = 0;
            while ((file = readdir(dir))) {
                if (!owned_file(file->d_name)) continue;
                char path[PATH_BYTES];
                struct stat st;
                if (!join_path(path, directory, file->d_name)) continue;
                if (stat(path, &st) != 0) { ok = false; break; }
                if (!S_ISREG(st.st_mode)) continue;
                total += st.st_size;
                if (strcmp(path, s_path) && (!oldest[0] || st.st_mtime < oldest_time ||
                    (st.st_mtime == oldest_time && strcmp(path, oldest) < 0))) {
                    strcpy(oldest, path);
                    oldest_time = st.st_mtime;
                }
                errno = 0;
            }
            int error = errno;
            if (closedir(dir) != 0 || error) ok = false;
            if (!ok) break;
            errno = 0;
        }
        int error = errno;
        if (closedir(root) != 0 || error) ok = false;
        if (!ok) { count_failure(error ? error : EIO); return false; }
        if (total <= (uint64_t)CONFIG_LOG_MANAGER_RETAIN_MIB * 1024 * 1024) return true;
        if (!oldest[0]) { count_failure(ENOSPC); return false; }
        if (unlink(oldest) != 0) { count_failure(errno); return false; }
    }
}

static void record_day(const char *line, char day[11])
{
    if (!strncmp(line, "[UNSYNCED]", 10)) strcpy(day, "unknown");
    else {
        memcpy(day, line + 1, 10);
        day[4] = day[7] = '_';
        day[10] = 0;
    }
}

static bool open_file(const char *first)
{
    if (sd_card_manager_acquire() != ESP_OK) return false;
    s_lease = true;
    char directory[PATH_BYTES], name[96];
    record_day(first, s_day);
    if (!join_path(directory, ROOT, s_day)) { close_file(false); return false; }
    if ((mkdir(ROOT, 0775) != 0 && errno != EEXIST) ||
        (mkdir(directory, 0775) != 0 && errno != EEXIST)) {
        count_failure(errno); close_file(false); return false;
    }
    int fd = -1;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        if (!strcmp(s_day, "unknown"))
            snprintf(name, sizeof(name), "boot_%016" PRIx64 "_%03" PRIu32 ".log", s.boot, s_sequence++);
        else
            snprintf(name, sizeof(name), "%.2s_%.2s_%.2s_boot_%016" PRIx64 "_%03" PRIu32 ".log",
                     first + 12, first + 15, first + 18, s.boot, s_sequence++);
        if (!join_path(s_path, directory, name)) break;
        fd = open(s_path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) { count_failure(errno); close_file(false); return false; }
    s_file = fdopen(fd, "wb");
    if (!s_file) {
        int error = errno;
        close(fd); count_failure(error); close_file(false); return false;
    }
    /* One fwrite per already-batched payload, no hidden per-line stdio buffer.
     * This also avoids retrying buffered data in fclose after a media failure.
     */
    if (setvbuf(s_file, NULL, _IONBF, 0) != 0) {
        count_failure(ENOMEM); close_file(false); return false;
    }
    s_file_bytes = 0;
    s_last_sync = esp_timer_get_time();
    s_retry_after = 0;
    return true;
}

static bool write_pending(void)
{
    if (!s.pending_bytes) return true;
    /* A producer can be preempted between capture and enqueue across SNTP.
     * Keep its original UNSYNCED timestamp, but never return to an unknown
     * file once this session has entered dated logging.
     */
    const char *file_time = s.batch;
    if (!strncmp(s.batch, "[UNSYNCED]", 10) && s_dated_prefix[0]) file_time = s_dated_prefix;
    char day[11];
    record_day(file_time, day);
    size_t marker_reserve = !strcmp(day, "unknown") ? 192 : 0;
    bool rotate = s_file && s_file_bytes + s.pending_bytes + marker_reserve >
                  (size_t)CONFIG_LOG_MANAGER_ROTATE_KIB * 1024;
    if (s_file && (strcmp(day, s_day) || rotate)) {
        if (!strcmp(s_day, "unknown") && strcmp(day, "unknown")) {
            char marker[192];
            int n = snprintf(marker, sizeof(marker),
                "[%.23s][+%06" PRIi64 "ms][I][LOG_MANAGER][TIME_SYNC][boot=%016" PRIx64 "] next=dated\n",
                file_time + 1, esp_timer_get_time() / 1000, s.boot);
            if (fwrite(marker, 1, n, s_file) != (size_t)n) {
                count_failure(errno); close_file(false); return false;
            }
            s_dirty = true;
        }
        if (!close_file(true)) return false;
        if (rotate && lock(LOCK_TICKS)) { ++s.stats.file_rotations; unlock(); }
    }
    if (!s_file && !open_file(file_time)) return false;
    /* Reserve retention headroom once per file, not a directory scan per batch. */
    if (!s_file_bytes && !retain((size_t)CONFIG_LOG_MANAGER_ROTATE_KIB * 1024)) {
        close_file(false); return false;
    }
    if (fwrite(s.batch, 1, s.pending_bytes, s_file) != s.pending_bytes) {
        count_failure(errno); close_file(false); return false;
    }
    s_file_bytes += s.pending_bytes;
    s_dirty = true;
    if (strcmp(day, "unknown") && file_time != s_dated_prefix) {
        memcpy(s_dated_prefix, file_time, 24);
        s_dated_prefix[24] = 0;
    }
    while (!lock(LOCK_TICKS)) taskYIELD();
    s.stats.storage_available = true;
    if (s_failed) ++s.stats.storage_recoveries;
    s_failed = false;
    s.stats.persisted_records += s.pending_records;
    s.pending_bytes = s.pending_records = 0;
    unlock();
    return true;
}

static size_t take_batch(void)
{
    if (!lock(LOCK_TICKS)) return 0;
    if (!s.pending_bytes) {
        char line[RECORD_BYTES], day[11], first_day[11] = "";
        size_t n;
        while ((n = log_buffer_peek(&s.ring, line, sizeof(line)))) {
            record_day(line, day);
            if (s.pending_bytes && (s.pending_bytes + n > BATCH_BYTES || strcmp(day, first_day))) break;
            if (!s.pending_bytes) strcpy(first_day, day);
            memcpy(s.batch + s.pending_bytes, line, n);
            s.pending_bytes += n;
            ++s.pending_records;
            log_buffer_pop(&s.ring);
        }
        s.batch[s.pending_bytes] = 0;
    }
    size_t count = s.pending_records;
    unlock();
    return count;
}

static bool enqueue_time_sync(void)
{
    if (!__atomic_load_n(&s_storage, __ATOMIC_RELAXED)) return false;
    char line[RECORD_BYTES], timestamp[32];
    struct tm local;
    if (time_manager_get_local_time(&local) != ESP_OK) return false;
    if (!strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local)) return false;
    if (!lock(LOCK_TICKS)) return false;
    if (!s.initialized || s.stopping) { unlock(); return false; }
    int n = snprintf(line, sizeof(line),
        "[%s.000][+%06" PRIi64 "ms][I][LOG_MANAGER][TIME_SYNC][boot=%016" PRIx64 "] source=time_manager\n",
        timestamp, esp_timer_get_time() / 1000, s.boot);
    ++s.stats.produced_records;
    s.stats.dropped_records += log_buffer_push(&s.ring, line, n);
    size_t buffered = s.ring.payload + s.pending_bytes;
    if (buffered > s.stats.peak_buffered_bytes) s.stats.peak_buffered_bytes = buffered;
    unlock();
    return true;
}

static void writer(void *context)
{
    (void)context;
    uint32_t bits = WAKE_ENV;
    int64_t last_write = esp_timer_get_time();
    bool saw_sync = false;
    for (;;) {
        bool mounted = sd_card_manager_is_mounted();
        bool synced = time_manager_is_synced();
        if (mounted && (bits & WAKE_ENV)) s_retry_after = 0;
        /* One control record makes the unknown->dated transition happen even
         * when application traffic is idle. It follows existing queued data.
         */
        if (synced && !saw_sync) {
            saw_sync = enqueue_time_sync();
        }
        if (!lock(LOCK_TICKS)) continue;
        s.stats.storage_available = mounted && !s_failed;
        s.stats.time_synchronized = synced;
        bool stop = s.stopping;
        uint32_t request = s.request;
        bool requested = request != s.completed;
        size_t backlog = s.ring.records + s.pending_records;
        size_t bytes = s.ring.payload + s.pending_bytes;
        unlock();
        int64_t now = esp_timer_get_time();
        bool due = now - last_write >= (int64_t)CONFIG_LOG_MANAGER_WRITE_MS * 1000;
        bool drain = requested || stop || (bits & (WAKE_ENV | WAKE_URGENT)) || due || bytes >= BATCH_BYTES;
        bool ok = true;
        if (!mounted) {
            if (!close_file(false)) s_durability_failed = true;
            ok = false;
        } else if (drain && (now >= s_retry_after || (bits & WAKE_ENV))) {
            /* Snapshot budget prevents a fast producer from starving control
             * requests. Producers never share the SD/stdio critical path.
             */
            while (backlog) {
                if (!sd_card_manager_is_mounted()) { ok = false; break; }
                size_t count = take_batch();
                if (!count || !write_pending()) { ok = false; break; }
                backlog = count >= backlog ? 0 : backlog - count;
                if (!sd_card_manager_is_mounted()) { ok = false; break; }
                if (esp_timer_get_time() - s_last_sync >= (int64_t)CONFIG_LOG_MANAGER_SYNC_MS * 1000 &&
                    !sync_file()) { close_file(false); ok = false; break; }
                taskYIELD();
            }
        } else if (backlog && (requested || stop)) ok = false;
        if (drain) last_write = esp_timer_get_time();
        mounted = sd_card_manager_is_mounted();
        if (!mounted) {
            if (!close_file(false)) s_durability_failed = true;
            ok = false;
        }
        if (mounted && (requested || stop || (bits & WAKE_URGENT) ||
            now - s_last_sync >= (int64_t)CONFIG_LOG_MANAGER_SYNC_MS * 1000)) {
            if (!sync_file()) { close_file(false); ok = false; }
        }
        if (stop && !close_file(mounted)) ok = false;
        if (requested || stop) {
            while (!lock(LOCK_TICKS)) taskYIELD();
            s.completed = request;
            s.request_result = (ok && !s_durability_failed) ? ESP_OK : ESP_FAIL;
            s_durability_failed = false;
            if (stop) { s.running = false; s.task = NULL; }
            unlock();
            xSemaphoreGive(s_done);
            if (stop) { vTaskDelete(NULL); return; }
        }
        bits = 0;
        /* Timeout is also a low-rate snapshot fallback if a hint was contended.
         * Keep independent sync and batch deadlines despite frequent hints.
         */
        now = esp_timer_get_time();
        int64_t wait_us = (int64_t)CONFIG_LOG_MANAGER_WRITE_MS * 1000 - (now - last_write);
        int64_t sync_us = (int64_t)CONFIG_LOG_MANAGER_SYNC_MS * 1000 - (now - s_last_sync);
        if (s_dirty && sync_us < wait_us) wait_us = sync_us;
        if (wait_us <= 0) wait_us = 1000;
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)((wait_us + 999) / 1000));
        xTaskNotifyWait(0, UINT32_MAX, &bits, ticks ? ticks : 1);
    }
}

static esp_err_t request_wait(bool stop, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount(), timeout = pdMS_TO_TICKS(timeout_ms);
    if (!lock(0)) return ESP_ERR_TIMEOUT;
    if (!s.initialized) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (!s.running) { unlock(); return stop ? ESP_OK : ESP_ERR_INVALID_STATE; }
    if (s.stopping && !stop) { unlock(); return ESP_ERR_INVALID_STATE; }
    bool already_stopping = s.stopping;
    if (stop) s.stopping = true;
    uint32_t request = already_stopping ? s.request : ++s.request;
    wake_locked(WAKE_REQUEST);
    unlock();
    for (;;) {
        if (lock(0)) {
            if (s.completed == request) {
                esp_err_t result = s.request_result;
                unlock(); return result;
            }
            unlock();
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) return ESP_ERR_TIMEOUT;
        xSemaphoreTake(s_done, timeout - elapsed);
    }
}

/* Functions ---------------------------------------------------------------- */
static void emit_record(esp_log_level_t level, const char *tag, const char *event,
                        const char *format, va_list args)
{
    if (xPortInIsrContext() || !tag || !event || !format) return;
    bool console = policy(&s_console, &s_console_level, level);
    bool storage = policy(&s_storage, &s_storage_level, level);
    if (!console && !storage) return;
    uint64_t boot = 0;
    if (lock(0)) {
        boot = s.boot;
        /* Provider diagnostics called by the writer must never feed its own
         * persistent sink (notably SD error reporting and lease cleanup). */
        storage = storage && s.initialized && !s.stopping &&
                  (!s.task || xTaskGetCurrentTaskHandle() != s.task);
        unlock();
    }
    else {
        if (storage && __atomic_load_n(&s_mutex, __ATOMIC_ACQUIRE))
            __atomic_fetch_add(&s_contention, 1, __ATOMIC_RELAXED);
        storage = false;
    }
    char line[RECORD_BYTES];
    bool truncated;
    size_t length = format_line(line, level, tag, event, format, args, boot, &truncated);
    if (storage) {
        if (lock(0)) {
            if (s.initialized && !s.stopping && s.boot == boot) {
                ++s.stats.produced_records;
                if (truncated) ++s.stats.truncated_records;
                size_t before = s.ring.payload;
                s.stats.dropped_records += log_buffer_push(&s.ring, line, length);
                size_t buffered = s.ring.payload + s.pending_bytes;
                if (buffered > s.stats.peak_buffered_bytes) s.stats.peak_buffered_bytes = buffered;
                if (before < BATCH_BYTES && s.ring.payload >= BATCH_BYTES) wake_locked(WAKE_DATA);
                if (level == ESP_LOG_ERROR) wake_locked(WAKE_URGENT);
            }
            unlock();
        } else __atomic_fetch_add(&s_contention, 1, __ATOMIC_RELAXED);
    }
    /* Public ESP-IDF sink API, verified against 6.0.1. One reserved sink tag
     * makes wrapper policy independent of library tag levels. The original
     * application tag is already in the structured line. No global hook.
     */
    if (console) esp_log_write(level, CONSOLE_SINK_TAG, "%s", line);
}

esp_err_t log_manager_init(void)
{
    app_log_register_backend(emit_record);
#if !CONFIG_LOG_MANAGER_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if ((uint64_t)CONFIG_LOG_MANAGER_ROTATE_KIB * 1024 + 192 >
        (uint64_t)CONFIG_LOG_MANAGER_RETAIN_MIB * 1024 * 1024) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) {
        s_done = xSemaphoreCreateBinaryStatic(&s_done_memory);
        SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&s_mutex_memory);
        __atomic_store_n(&s_mutex, mutex, __ATOMIC_RELEASE);
    }
    if (!lock(LOCK_TICKS)) return ESP_ERR_TIMEOUT;
    if (s.initialized) { unlock(); return ESP_OK; }
    const size_t capacity = (size_t)CONFIG_LOG_MANAGER_BUFFER_KIB * 1024;
    /* PSRAM_REQUIRED: one lifetime allocation, no internal fallback. */
    void *memory = heap_caps_malloc(capacity + BATCH_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory || !esp_ptr_external_ram(memory)) {
        heap_caps_free(memory); unlock(); return ESP_ERR_NO_MEM;
    }
    s = (manager_t){0};
    log_buffer_init(&s.ring, memory, capacity);
    s.batch = (char *)memory + capacity;
    s.stats.buffer_capacity = capacity;
    s.boot = ((uint64_t)esp_random() << 32) | esp_random();
    s.initialized = true;
    __atomic_store_n(&s_contention, 0, __ATOMIC_RELAXED);
    s_sequence = 0;
    s_retry_after = 0;
    s_failed = false;
    s_dated_prefix[0] = 0;
    s_durability_failed = false;
    unlock();
    log_manager_set_console_level(__atomic_load_n(&s_console_level, __ATOMIC_RELAXED));
    return ESP_OK;
#endif
}

esp_err_t log_manager_start(void)
{
    if (!lock(LOCK_TICKS)) return ESP_ERR_INVALID_STATE;
    if (!s.initialized || (s.running && s.stopping)) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (s.running) { unlock(); return ESP_OK; }
    s.stopping = false;
    s.running = true;
    /* ESP-IDF xTaskCreate uses internal stack allocation; no project-wide
     * PSRAM task override is applied to this component. Priority 2, unpinned.
     */
    if (xTaskCreate(writer, "log_writer", WRITER_STACK_BYTES, NULL, 2, &s.task) != pdPASS) {
        s.running = false; s.task = NULL; unlock(); return ESP_ERR_NO_MEM;
    }
    unlock();
    return ESP_OK;
}

esp_err_t log_manager_flush(uint32_t timeout_ms) { return request_wait(false, timeout_ms); }
esp_err_t log_manager_stop(uint32_t timeout_ms) { return request_wait(true, timeout_ms); }

esp_err_t log_manager_deinit(void)
{
    if (!s_mutex) return ESP_OK;
    if (!lock(LOCK_TICKS)) return ESP_ERR_TIMEOUT;
    if (s.running) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (s.initialized) {
        s.stats.dropped_records += s.ring.records + s.pending_records;
        heap_caps_free(s.ring.data);
        log_buffer_init(&s.ring, NULL, 0);
        s.batch = NULL;
        s.pending_bytes = s.pending_records = 0;
        s.initialized = false;
        s.stats.buffer_capacity = 0;
    }
    unlock();
    return ESP_OK;
}

esp_err_t log_manager_get_stats(log_manager_stats_t *stats)
{
    if (!stats) return ESP_ERR_INVALID_ARG;
    if (!lock(LOCK_TICKS)) return ESP_ERR_TIMEOUT;
    *stats = s.stats;
    stats->contention_drops = __atomic_load_n(&s_contention, __ATOMIC_RELAXED);
    stats->produced_records += stats->contention_drops;
    stats->dropped_records += stats->contention_drops;
    stats->buffered_bytes = s.ring.payload + s.pending_bytes;
    unlock();
    return ESP_OK;
}

void log_manager_set_console_enabled(bool enabled) { __atomic_store_n(&s_console, enabled, __ATOMIC_RELAXED); }
void log_manager_set_storage_enabled(bool enabled) { __atomic_store_n(&s_storage, enabled, __ATOMIC_RELAXED); }
void log_manager_set_console_level(esp_log_level_t level)
{
    if (level >= ESP_LOG_NONE && level <= ESP_LOG_VERBOSE) {
        __atomic_store_n(&s_console_level, level, __ATOMIC_RELAXED);
#if CONFIG_LOG_DYNAMIC_LEVEL_CONTROL && !CONFIG_LOG_TAG_LEVEL_IMPL_NONE
        /* Never use "*": normal ESP-IDF/library log policies stay untouched. */
        esp_log_level_set(CONSOLE_SINK_TAG, level);
#endif
    }
}
void log_manager_set_storage_level(esp_log_level_t level)
{
    if (level >= ESP_LOG_NONE && level <= ESP_LOG_VERBOSE) __atomic_store_n(&s_storage_level, level, __ATOMIC_RELAXED);
}
void log_manager_notify_environment_changed(void)
{
    if (lock(0)) { wake_locked(WAKE_ENV); unlock(); }
}
