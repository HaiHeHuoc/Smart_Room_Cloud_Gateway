/* Includes ----------------------------------------------------------------- */
#include "log_manager.h"
#include "app_log.h"
#include "app_log_backend.h"
#include "log_buffer.h"
#include "board_config.h"
#include "sd_card_manager.h"
#include "time_manager.h"
#include "voice_recording_critical.h"
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
#define WAKE_RECORDING_CRITICAL 16U
#define LOCK_TICKS pdMS_TO_TICKS(20)
#define WRITER_STACK_BYTES 6144
#define CONSOLE_SINK_TAG "APP_LOG_SINK"
#define ARCHIVE_SCAN_MAX 64U
#define ARCHIVE_DIRECTORY_SCAN_MAX 64U
#define ARCHIVE_LINE_BYTES RECORD_BYTES

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
/* INTERNAL_REQUIRED: locks and shared state. Never place these in PSRAM. */
#if CONFIG_LOG_MANAGER_ENABLE
static StaticSemaphore_t s_mutex_memory, s_done_memory, s_archive_read_gate_memory;
#endif
static SemaphoreHandle_t s_mutex, s_done, s_archive_read_gate;
static manager_t s;
static uint32_t s_contention;
/* Lock-free fallback copy used only for console formatting when the producer
 * cannot take s_mutex. Two 32-bit words avoid relying on 64-bit atomics on
 * ESP32-S3. They change only during serialized successful init(). */
static uint32_t s_boot_hi, s_boot_lo;
static bool s_console = true, s_storage = true;
static bool s_start_failed;
static int s_console_level = CONFIG_LOG_MANAGER_CONSOLE_LEVEL;
static int s_storage_level = CONFIG_LOG_MANAGER_STORAGE_LEVEL;
/* Writer-only file state; every filesystem call below runs in this context. */
static FILE *s_file;
static bool s_lease, s_dirty, s_failed;
/* Preserve an uncertain durability outcome across background close/remount
 * until a management request has observed it. An empty ring is not a sync. */
static bool s_durability_failed;
static uint64_t s_unsynced_records;
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

/* Reader holds this only for one bounded archive request. Retention defers a
 * delete instead of racing a browser-owned FILE handle. */
static bool archive_read_gate_take(TickType_t ticks)
{
    SemaphoreHandle_t gate = __atomic_load_n(
        &s_archive_read_gate, __ATOMIC_ACQUIRE);
    return gate && xSemaphoreTake(gate, ticks) == pdTRUE;
}

static void archive_read_gate_release(void)
{
    xSemaphoreGive(s_archive_read_gate);
}

static void wake_locked(uint32_t bits)
{
    if (s.task) xTaskNotify(s.task, bits, eSetBits);
}

/* The common runtime service invokes listeners from the voice-uplink task only
 * after releasing its lock. A task notification is the sole side effect here:
 * no logger mutex, console formatting, allocation, or SD/VFS work occurs on
 * the capture path. */
static void log_manager_recording_critical_listener(
    bool active,
    uint32_t transition_sequence,
    void *context)
{
    (void)active;
    (void)transition_sequence;
    (void)context;
    TaskHandle_t writer_task = __atomic_load_n(
        &s.task, __ATOMIC_ACQUIRE);
    if (writer_task != NULL) {
        (void)xTaskNotify(
            writer_task,
            WAKE_RECORDING_CRITICAL,
            eSetBits);
    }
}

static uint64_t boot_snapshot(void)
{
    uint32_t hi = __atomic_load_n(&s_boot_hi, __ATOMIC_RELAXED);
    uint32_t lo = __atomic_load_n(&s_boot_lo, __ATOMIC_RELAXED);
    return ((uint64_t)hi << 32) | lo;
}

static bool policy(bool *enabled, int *maximum, esp_log_level_t level)
{
    return level > ESP_LOG_NONE && level <= ESP_LOG_VERBOSE &&
           __atomic_load_n(enabled, __ATOMIC_RELAXED) &&
           (int)level <= __atomic_load_n(maximum, __ATOMIC_RELAXED);
}

/* Frontend-only policy query. It deliberately reads only atomic sink policy;
 * lifecycle/storage readiness remains the backend's concern. This lets
 * APP_LOG macros skip argument evaluation when neither configured sink can
 * consume the requested level without taking the logger mutex. A failed writer
 * start is a deliberate persistence fail-closed state until start succeeds. */
static bool should_emit(esp_log_level_t level)
{
    bool console = policy(&s_console, &s_console_level, level);
    bool storage = policy(&s_storage, &s_storage_level, level) &&
                   !__atomic_load_n(&s_start_failed, __ATOMIC_RELAXED);
    return console || storage;
}

/* READY is the logical VFS state. sd_card_manager_is_mounted() also returns
 * false during its short idle health-check reservation, which must not be
 * mistaken for card loss by the logger. Actual file access is still protected
 * by sd_card_manager_acquire(), so a health check can temporarily reject I/O.
 */
static bool storage_ready(void)
{
    sd_card_manager_status_t status = {0};
    return sd_card_manager_get_status(&status) == ESP_OK &&
           status.state == SD_CARD_MANAGER_STATE_READY;
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

static void mark_unsynced_uncertain(void)
{
    if (!s_unsynced_records) return;
    while (!lock(LOCK_TICKS)) taskYIELD();
    s.stats.durability_uncertain_records += s_unsynced_records;
    s_unsynced_records = 0;
    unlock();
}

/* Close only the current stdio handle and release its lease. The segment
 * identity, byte count and dirty state remain valid so later batches/syncs can
 * reopen the same file. This is the key ownership rule that leaves idle gaps
 * with zero SD leases for sd_card_manager health checks and hot-remove detect.
 */
static bool release_file_handle(void)
{
    bool ok = true;
    if (s_file) {
        if (fclose(s_file) != 0) {
            count_failure(errno);
            ok = false;
        }
        s_file = NULL;
    }
    if (s_lease) {
        sd_card_manager_release();
        s_lease = false;
    }
    return ok;
}

static void clear_segment(bool durability_uncertain)
{
    if (durability_uncertain && s_dirty) {
        s_durability_failed = true;
        mark_unsynced_uncertain();
    }
    s_path[0] = 0;
    s_day[0] = 0;
    s_file_bytes = 0;
    s_dirty = false;
}

static bool reopen_file(void)
{
    if (!s_path[0]) return false;
    if (sd_card_manager_acquire() != ESP_OK) return false;
    s_lease = true;

    int fd = open(s_path, O_WRONLY | O_APPEND);
    if (fd < 0) {
        int error = errno;
        count_failure(error);
        sd_card_manager_release();
        s_lease = false;
        clear_segment(true);
        return false;
    }

    s_file = fdopen(fd, "ab");
    if (!s_file) {
        int error = errno;
        close(fd);
        count_failure(error);
        sd_card_manager_release();
        s_lease = false;
        clear_segment(true);
        return false;
    }

    if (setvbuf(s_file, NULL, _IONBF, 0) != 0) {
        count_failure(ENOMEM);
        (void)release_file_handle();
        clear_segment(true);
        return false;
    }
    return true;
}

static bool sync_file(void)
{
    if (!s_dirty) return true;
    if (!s_file && !reopen_file()) return false;
    if (fflush(s_file) != 0 || fsync(fileno(s_file)) != 0) {
        if (lock(LOCK_TICKS)) {
            ++s.stats.durability_sync_failures;
            unlock();
        }
        count_failure(errno);
        return false;
    }
    if (lock(LOCK_TICKS)) {
        ++s.stats.durability_syncs;
        s.stats.durable_records += s_unsynced_records;
        s_unsynced_records = 0;
        unlock();
    }
    s_dirty = false;
    s_last_sync = esp_timer_get_time();
    if (!release_file_handle()) {
        clear_segment(false);
        return false;
    }
    return true;
}

static bool close_file(bool sync)
{
    /* A READY manager can temporarily reject new leases while its idle health
     * check owns the card. Preserve the segment and let the caller retry rather
     * than converting that short reservation into a false durability failure.
     */
    if (sync && s_dirty && !s_file && storage_ready() &&
        !sd_card_manager_is_mounted()) {
        return false;
    }
    bool uncertain = !sync && s_dirty;
    if (uncertain) s_durability_failed = true;
    bool ok = !sync || sync_file();
    if (s_file && !release_file_handle()) ok = false;
    if (s_lease) {
        sd_card_manager_release();
        s_lease = false;
    }
    clear_segment(uncertain || !ok);
    if (!ok) s_durability_failed = true;
    return ok;
}

static bool park_file(void)
{
    if (release_file_handle()) return true;
    clear_segment(true);
    return false;
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

static uint64_t archive_hash(const char *path)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (; *path; ++path) {
        hash ^= (uint8_t)*path;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void archive_id(char out[LOG_MANAGER_ARCHIVE_ID_LEN + 1U], const char *path)
{
    (void)snprintf(out, LOG_MANAGER_ARCHIVE_ID_LEN + 1U, "%016" PRIx64,
                   archive_hash(path));
}

static bool archive_insert(log_manager_archive_list_t *archives,
                           const char *path, const struct stat *st)
{
    if ((archives == NULL) || (path == NULL) || (st == NULL)) return false;
    log_manager_archive_t entry = {
        .size_bytes = (uint64_t)st->st_size,
        .time_named = strncmp(strrchr(path, '/') + 1, "boot_", 5) != 0,
        .sort_time = (int64_t)st->st_mtime,
    };
    archive_id(entry.id, path);
    for (uint8_t index = 0U; index < archives->count; ++index) {
        if (strcmp(archives->archives[index].id, entry.id) == 0) return false;
    }
    uint8_t insert_at = archives->count;
    while ((insert_at > 0U) &&
           ((entry.sort_time > archives->archives[insert_at - 1U].sort_time) ||
            ((entry.sort_time == archives->archives[insert_at - 1U].sort_time) &&
             (strcmp(entry.id, archives->archives[insert_at - 1U].id) < 0)))) --insert_at;
    if (archives->count < LOG_MANAGER_ARCHIVE_LIST_MAX) {
        for (uint8_t index = archives->count; index > insert_at; --index)
            archives->archives[index] = archives->archives[index - 1U];
        archives->archives[insert_at] = entry;
        ++archives->count;
    } else {
        archives->truncated = true;
        if (insert_at < archives->count) {
            for (uint8_t index = archives->count - 1U; index > insert_at; --index)
                archives->archives[index] = archives->archives[index - 1U];
            archives->archives[insert_at] = entry;
        }
    }
    return true;
}

static esp_err_t archive_scan(log_manager_archive_list_t *archives,
                              const char *requested_id, char found[PATH_BYTES])
{
    if (archives) *archives = (log_manager_archive_list_t){ .available = true };
    if (found) found[0] = '\0';
    if (voice_recording_critical_is_active()) return ESP_ERR_TIMEOUT;
    char active[PATH_BYTES] = {0};
    if (!lock(LOCK_TICKS)) return ESP_ERR_TIMEOUT;
    (void)snprintf(active, sizeof(active), "%s", s_path);
    unlock();
    if (sd_card_manager_acquire() != ESP_OK) return ESP_ERR_INVALID_STATE;

    esp_err_t result = ESP_OK;
    DIR *root = opendir(ROOT);
    if (!root) {
        int error = errno;
        sd_card_manager_release();
        if (sd_card_manager_is_vfs_media_error(error)) sd_card_manager_report_io_error(ESP_FAIL);
        return ESP_FAIL;
    }
    uint16_t scanned = 0U;
    uint16_t directories_scanned = 0U;
    bool scan_limit_reached = false;
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL && result == ESP_OK &&
           !scan_limit_reached) {
        if (!owned_directory(entry->d_name)) continue;
        if (++directories_scanned > ARCHIVE_DIRECTORY_SCAN_MAX) {
            if (archives) archives->truncated = true;
            scan_limit_reached = true;
            break;
        }
        char directory[PATH_BYTES];
        if (!join_path(directory, ROOT, entry->d_name)) continue;
        DIR *dir = opendir(directory);
        if (!dir) { result = ESP_FAIL; break; }
        struct dirent *file;
        while ((file = readdir(dir)) != NULL) {
            if (!owned_file(file->d_name)) continue;
            if (++scanned > ARCHIVE_SCAN_MAX) {
                if (archives) archives->truncated = true;
                scan_limit_reached = true;
                break;
            }
            char path[PATH_BYTES], id[LOG_MANAGER_ARCHIVE_ID_LEN + 1U];
            struct stat st;
            if (!join_path(path, directory, file->d_name) || stat(path, &st) != 0 ||
                !S_ISREG(st.st_mode)) continue;
            if (strcmp(path, active) == 0) continue;
            archive_id(id, path);
            if (requested_id && strcmp(id, requested_id) == 0) {
                if (found && found[0]) { result = ESP_ERR_INVALID_RESPONSE; break; }
                if (found) (void)snprintf(found, PATH_BYTES, "%s", path);
            }
            if (archives && !archive_insert(archives, path, &st)) {
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
        }
        if (closedir(dir) != 0 && result == ESP_OK) result = ESP_FAIL;
    }
    if (closedir(root) != 0 && result == ESP_OK) result = ESP_FAIL;
    sd_card_manager_release();
    return result;
}

static bool archive_parse_line(const char *line, log_manager_public_record_t *record)
{
    if ((line == NULL) || (record == NULL)) return false;
    const char *cursor = line;
    const char *end = strchr(cursor, ']');
    if ((cursor[0] != '[') || (end == NULL)) return false;
    *record = (log_manager_public_record_t){0};
    if (strncmp(cursor + 1, "UNSYNCED", 8) == 0 && end == cursor + 9) {
        record->time_valid = false;
    } else {
        if ((size_t)(end - cursor - 1) != LOG_MANAGER_PUBLIC_TIMESTAMP_MAX_LEN) return false;
        for (size_t index = 0U; index < LOG_MANAGER_PUBLIC_TIMESTAMP_MAX_LEN; ++index)
            if ((unsigned char)cursor[1U + index] < 0x20U) return false;
        memcpy(record->timestamp, cursor + 1, LOG_MANAGER_PUBLIC_TIMESTAMP_MAX_LEN);
        record->time_valid = true;
    }
    cursor = end + 1;
    unsigned long long uptime = 0ULL;
    char severity = 0;
    char tag[LOG_MANAGER_PUBLIC_TAG_MAX_LEN + 1U] = {0};
    char event[LOG_MANAGER_PUBLIC_EVENT_MAX_LEN + 1U] = {0};
    if (sscanf(cursor, "[+%llums][%c][%24[^]]][%40[^]]][boot=%*16[0-9a-f]]",
               &uptime, &severity, tag, event) != 4) return false;
    const char *levels = "VDIWE";
    const char *mapped[] = { "verbose", "debug", "info", "warn", "error" };
    const char *level = strchr(levels, severity);
    if (level == NULL) return false;
    for (const char *field = tag; *field; ++field)
        if (!((*field >= 'A' && *field <= 'Z') || (*field >= '0' && *field <= '9') ||
              *field == '_' || *field == '-')) return false;
    for (const char *field = event; *field; ++field)
        if (!((*field >= 'A' && *field <= 'Z') || (*field >= '0' && *field <= '9') ||
              *field == '_' || *field == '-')) return false;
    record->uptime_ms = uptime;
    (void)snprintf(record->level, sizeof(record->level), "%s", mapped[level - levels]);
    (void)snprintf(record->tag, sizeof(record->tag), "%s", tag);
    (void)snprintf(record->event, sizeof(record->event), "%s", event);
    return true;
}

/* Constant-memory, two-level scan. Metadata operations are writer-only and
 * hold the active file's lease. Re-scan after each deletion: simple V1 policy.
 * A scan/delete error halts this pass instead of risking unrelated files.
 */
static bool retain(size_t incoming)
{
    /* A bounded HTTP archive read wins over cleanup. The next file-open
     * retries retention; the writer/ring lock is never held for this wait. */
    if (!archive_read_gate_take(0U)) return true;
    for (;;) {
        uint64_t total = incoming;
        char oldest[PATH_BYTES] = "";
        time_t oldest_time = 0;
        bool ok = true;
        DIR *root = opendir(ROOT);
        if (!root) { count_failure(errno); archive_read_gate_release(); return false; }
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
                    (void)snprintf(oldest, sizeof(oldest), "%s", path);
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
        if (!ok) { count_failure(error ? error : EIO); archive_read_gate_release(); return false; }
        if (total <= (uint64_t)CONFIG_LOG_MANAGER_RETAIN_MIB * 1024 * 1024) {
            archive_read_gate_release(); return true;
        }
        if (!oldest[0]) { count_failure(ENOSPC); archive_read_gate_release(); return false; }
        if (unlink(oldest) != 0) { count_failure(errno); archive_read_gate_release(); return false; }
    }
}

static void record_day(const char *line, char day[11])
{
    if (!strncmp(line, "[UNSYNCED]", 10)) {
        (void)snprintf(day, 11, "%s", "unknown");
    }
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

/* Consume bytes/records from the retry batch after an I/O attempt. Caller
 * holds s_mutex. A partial physical record is dropped through its terminating
 * newline so the next segment always begins at a record boundary. */
static void consume_pending_locked(size_t bytes, size_t records)
{
    if (bytes > s.pending_bytes) bytes = s.pending_bytes;
    if (records > s.pending_records) records = s.pending_records;
    if (bytes && bytes < s.pending_bytes)
        memmove(s.batch, s.batch + bytes, s.pending_bytes - bytes);
    s.pending_bytes -= bytes;
    s.pending_records -= records;
    if (s.batch) s.batch[s.pending_bytes] = 0;
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
    bool segment_exists = s_path[0] != 0;
    bool rotate = segment_exists && s_file_bytes + s.pending_bytes + marker_reserve >
                  (size_t)CONFIG_LOG_MANAGER_ROTATE_KIB * 1024;
    if (segment_exists && (strcmp(day, s_day) || rotate)) {
        if (!s_file && !reopen_file()) return false;
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
    if (!s_path[0]) {
        if (!open_file(file_time)) return false;
    } else if (!s_file && !reopen_file()) {
        return false;
    }
    /* Reserve retention headroom once per file, not a directory scan per batch. */
    if (!s_file_bytes && !retain((size_t)CONFIG_LOG_MANAGER_ROTATE_KIB * 1024)) {
        close_file(false); return false;
    }

    size_t attempted_bytes = s.pending_bytes;
    size_t attempted_records = s.pending_records;
    size_t written = fwrite(s.batch, 1, attempted_bytes, s_file);
    if (written != attempted_bytes) {
        if (written > attempted_bytes) written = attempted_bytes;
        size_t complete_bytes = 0, complete_records = 0;
        for (size_t i = 0; i < written; ++i) {
            if (s.batch[i] == '\n') {
                complete_bytes = i + 1;
                ++complete_records;
            }
        }
        size_t consumed_bytes = complete_bytes;
        size_t consumed_records = complete_records;
        size_t dropped_partial = 0;
        if (written > complete_bytes && complete_bytes < attempted_bytes) {
            const char *end = memchr(s.batch + complete_bytes, '\n', attempted_bytes - complete_bytes);
            if (end) {
                consumed_bytes = (size_t)(end - s.batch) + 1;
                consumed_records = complete_records + 1;
                dropped_partial = 1;
            } else {
                consumed_bytes = attempted_bytes;
                consumed_records = attempted_records;
                dropped_partial = attempted_records - complete_records;
            }
        }
        if (written) {
            s_file_bytes += written;
            s_dirty = true;
        }
        while (!lock(LOCK_TICKS)) taskYIELD();
        if (written) ++s.stats.partial_write_events;
        s.stats.persisted_records += complete_records;
        s.stats.dropped_records += dropped_partial;
        s_unsynced_records += complete_records;
        consume_pending_locked(consumed_bytes, consumed_records);
        unlock();
        count_failure(errno ? errno : EIO);
        close_file(false);
        return false;
    }

    s_file_bytes += attempted_bytes;
    s_dirty = true;
    if (strcmp(day, "unknown") && file_time != s_dated_prefix) {
        memcpy(s_dated_prefix, file_time, 24);
        s_dated_prefix[24] = 0;
    }
    /* Account complete records before closing the handle. If fclose itself
     * fails, those fwrite-accepted records remain correctly classified as
     * persisted-but-durability-uncertain instead of disappearing from stats. */
    while (!lock(LOCK_TICKS)) taskYIELD();
    s.stats.persisted_records += attempted_records;
    s_unsynced_records += attempted_records;
    consume_pending_locked(attempted_bytes, attempted_records);
    unlock();

    /* Do not keep a file lease while waiting for the next batch/sync deadline.
     * fclose() is not a durability boundary here; s_dirty stays set and the
     * periodic/urgent sync path reopens this same segment and fsyncs it.
     */
    if (!park_file()) return false;
    while (!lock(LOCK_TICKS)) taskYIELD();
    s.stats.storage_available = true;
    if (s_failed) ++s.stats.storage_recoveries;
    s_failed = false;
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
            if (!s.pending_bytes) {
                (void)snprintf(first_day, sizeof(first_day), "%s", day);
            }
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
    if (!__atomic_load_n(&s_storage, __ATOMIC_RELAXED) ||
        __atomic_load_n(&s_start_failed, __ATOMIC_RELAXED)) return false;
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
    wake_locked(WAKE_DATA);
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
        /* Never interrupt a filesystem transaction. This test is reached only
         * at the writer loop's safe point, after any write/close operation has
         * returned. Producer records, including ERROR, remain queued in the
         * bounded PSRAM ring and console output remains unchanged. */
        if (voice_recording_critical_is_active()) {
            bool parked = true;
            if (s_file != NULL) {
                parked = park_file();
            }
            if (lock(LOCK_TICKS)) {
                ++s.stats.recording_critical_deferred_cycles;
                unlock();
            }
            if (!parked) {
                s_durability_failed = true;
            }
            uint32_t ignored_bits = 0U;
            (void)xTaskNotifyWait(
                0U,
                UINT32_MAX,
                &ignored_bits,
                portMAX_DELAY);
            bits = ignored_bits;
            continue;
        }

        bool mounted = storage_ready();
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
                if (!storage_ready()) { ok = false; break; }
                size_t count = take_batch();
                if (!count || !write_pending()) { ok = false; break; }
                backlog = count >= backlog ? 0 : backlog - count;
                if (!storage_ready()) { ok = false; break; }
                if (esp_timer_get_time() - s_last_sync >= (int64_t)CONFIG_LOG_MANAGER_SYNC_MS * 1000 &&
                    !sync_file()) {
                    if (s_failed || !storage_ready()) close_file(false);
                    ok = false;
                    break;
                }
                taskYIELD();
            }
        } else if (backlog && (requested || stop)) ok = false;
        if (drain) last_write = esp_timer_get_time();
        mounted = storage_ready();
        if (!mounted) {
            if (!close_file(false)) s_durability_failed = true;
            ok = false;
        }
        if (mounted && (requested || stop || (bits & WAKE_URGENT) ||
            now - s_last_sync >= (int64_t)CONFIG_LOG_MANAGER_SYNC_MS * 1000)) {
            if (!sync_file()) {
                if (s_failed || !storage_ready()) close_file(false);
                ok = false;
            }
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
        /* Fully idle writer is notification-driven. The first queued record
         * explicitly wakes it; below-threshold backlog then uses WRITE_MS as
         * its drain deadline, while dirty data waits only for the sync deadline.
         * This removes the former unconditional one-second idle snapshot poll. */
        now = esp_timer_get_time();
        TickType_t ticks = portMAX_DELAY;
        if (backlog) {
            int64_t wait_us = (int64_t)CONFIG_LOG_MANAGER_WRITE_MS * 1000 - (now - last_write);
            if (wait_us <= 0) wait_us = 1000;
            ticks = pdMS_TO_TICKS((uint32_t)((wait_us + 999) / 1000));
            if (!ticks) ticks = 1;
        } else if (s_dirty) {
            int64_t wait_us = (int64_t)CONFIG_LOG_MANAGER_SYNC_MS * 1000 - (now - s_last_sync);
            if (wait_us <= 0) wait_us = 1000;
            ticks = pdMS_TO_TICKS((uint32_t)((wait_us + 999) / 1000));
            if (!ticks) ticks = 1;
        }
        xTaskNotifyWait(0, UINT32_MAX, &bits, ticks);
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
    bool storage = policy(&s_storage, &s_storage_level, level) &&
                   !__atomic_load_n(&s_start_failed, __ATOMIC_RELAXED);
    if (!console && !storage) return;
    /* Console formatting must keep the established session identity even when
     * this non-blocking producer loses the logger mutex. Before first init the
     * snapshot is intentionally zero, matching the existing early-console path. */
    uint64_t boot = boot_snapshot();
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
            if (s.initialized && !s.stopping &&
                !__atomic_load_n(&s_start_failed, __ATOMIC_RELAXED) && s.boot == boot) {
                ++s.stats.produced_records;
                if (truncated) ++s.stats.truncated_records;
                bool was_empty = s.ring.records == 0 && s.pending_records == 0;
                size_t before = s.ring.payload;
                s.stats.dropped_records += log_buffer_push(&s.ring, line, length);
                size_t buffered = s.ring.payload + s.pending_bytes;
                if (buffered > s.stats.peak_buffered_bytes) s.stats.peak_buffered_bytes = buffered;
                if (was_empty || (before < BATCH_BYTES && s.ring.payload >= BATCH_BYTES))
                    wake_locked(WAKE_DATA);
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
    app_log_register_backend(emit_record, should_emit);
#if !CONFIG_LOG_MANAGER_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if ((uint64_t)CONFIG_LOG_MANAGER_ROTATE_KIB * 1024 + 192 >
        (uint64_t)CONFIG_LOG_MANAGER_RETAIN_MIB * 1024 * 1024) return ESP_ERR_INVALID_ARG;
    if (!s_mutex) {
        s_done = xSemaphoreCreateBinaryStatic(&s_done_memory);
        SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&s_mutex_memory);
        s_archive_read_gate = xSemaphoreCreateMutexStatic(&s_archive_read_gate_memory);
        __atomic_store_n(&s_mutex, mutex, __ATOMIC_RELEASE);
    }
    const esp_err_t listener_ret =
        voice_recording_critical_register_listener(
            log_manager_recording_critical_listener,
            NULL);
    if (listener_ret != ESP_OK) return listener_ret;
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
    __atomic_store_n(&s_boot_hi, (uint32_t)(s.boot >> 32), __ATOMIC_RELAXED);
    __atomic_store_n(&s_boot_lo, (uint32_t)s.boot, __ATOMIC_RELAXED);
    s.initialized = true;
    __atomic_store_n(&s_contention, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_start_failed, false, __ATOMIC_RELAXED);
    s_sequence = 0;
    s_retry_after = 0;
    s_failed = false;
    s_dated_prefix[0] = 0;
    s_durability_failed = false;
    s_unsynced_records = 0;
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
    /* Component-local xTaskCreate override places this audited SD/VFS worker
     * stack and TCB in PSRAM. Priority 2, unpinned. */
    if (xTaskCreate(writer, "log_writer", WRITER_STACK_BYTES, NULL, 2, &s.task) != pdPASS) {
        s.running = false;
        s.task = NULL;
        ++s.stats.writer_start_failures;
        s.stats.storage_available = false;
        __atomic_store_n(&s_start_failed, true, __ATOMIC_RELAXED);
        unlock();
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&s_start_failed, false, __ATOMIC_RELAXED);
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
        s_unsynced_records = 0;
        __atomic_store_n(&s_start_failed, false, __ATOMIC_RELAXED);
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

esp_err_t log_manager_list_archives(log_manager_archive_list_t *archives)
{
    if (archives == NULL) return ESP_ERR_INVALID_ARG;
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    return archive_scan(archives, NULL, NULL);
}

static bool archive_id_valid(const char *id)
{
    if ((id == NULL) || (strlen(id) != LOG_MANAGER_ARCHIVE_ID_LEN)) return false;
    for (size_t index = 0U; index < LOG_MANAGER_ARCHIVE_ID_LEN; ++index) {
        if (!((id[index] >= '0' && id[index] <= '9') ||
              (id[index] >= 'a' && id[index] <= 'f'))) return false;
    }
    return true;
}

esp_err_t log_manager_read_archive(const char *archive_id, uint64_t offset,
                                   log_manager_archive_page_t *page)
{
    if ((page == NULL) || !archive_id_valid(archive_id)) return ESP_ERR_INVALID_ARG;
    if (!s.initialized) return ESP_ERR_INVALID_STATE;
    *page = (log_manager_archive_page_t){
        .available = true,
        .next_offset_valid = true,
        .details_omitted = true,
        .offset = offset,
    };
    if (!archive_read_gate_take(LOCK_TICKS)) return ESP_ERR_TIMEOUT;
    char path[PATH_BYTES] = {0};
    esp_err_t result = archive_scan(NULL, archive_id, path);
    if (result != ESP_OK) { archive_read_gate_release(); return result; }
    if (!path[0]) { archive_read_gate_release(); return ESP_ERR_NOT_FOUND; }
    if (voice_recording_critical_is_active()) { archive_read_gate_release(); return ESP_ERR_TIMEOUT; }
    if (offset > INT32_MAX) { archive_read_gate_release(); return ESP_ERR_INVALID_SIZE; }
    if (sd_card_manager_acquire() != ESP_OK) { archive_read_gate_release(); return ESP_ERR_INVALID_STATE; }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        int error = errno;
        sd_card_manager_release();
        archive_read_gate_release();
        if (error == ENOENT) return ESP_ERR_NOT_FOUND;
        if (sd_card_manager_is_vfs_media_error(error)) sd_card_manager_report_io_error(ESP_FAIL);
        return ESP_FAIL;
    }
    if ((offset > 0U) &&
        ((fseek(file, (long)(offset - 1U), SEEK_SET) != 0) || fgetc(file) != '\n')) {
        (void)fclose(file);
        sd_card_manager_release();
        archive_read_gate_release();
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        (void)fclose(file);
        sd_card_manager_release();
        archive_read_gate_release();
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t scanned = 0U;
    bool next_offset_valid = true;
    char line[ARCHIVE_LINE_BYTES] = {0};
    while ((page->record_count < LOG_MANAGER_ARCHIVE_PAGE_MAX_RECORDS) &&
           (scanned < LOG_MANAGER_ARCHIVE_PAGE_MAX_SCAN_BYTES) &&
           fgets(line, sizeof(line), file) != NULL) {
        const size_t length = strlen(line);
        scanned += (uint32_t)length;
        if ((length == 0U) || (line[length - 1U] != '\n')) {
            ++page->malformed_record_count;
            bool line_finished = false;
            int character;
            while (scanned < LOG_MANAGER_ARCHIVE_PAGE_MAX_SCAN_BYTES &&
                   (character = fgetc(file)) != EOF) {
                ++scanned;
                if (character == '\n') {
                    line_finished = true;
                    break;
                }
            }
            if (!line_finished) next_offset_valid = false;
            continue;
        }
        if (archive_parse_line(line, &page->records[page->record_count])) {
            ++page->record_count;
        } else {
            ++page->malformed_record_count;
        }
    }
    const long position = ftell(file);
    page->next_offset_valid = next_offset_valid && position >= 0;
    page->next_offset = page->next_offset_valid ? (uint64_t)position : offset;
    page->eof = !page->next_offset_valid || (feof(file) != 0);
    const int close_result = fclose(file);
    sd_card_manager_release();
    archive_read_gate_release();
    if (close_result != 0) return ESP_FAIL;
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
