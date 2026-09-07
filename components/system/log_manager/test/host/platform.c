#include "platform.h"
#include <assert.h>
#include <errno.h>
#include <io.h>
#include <direct.h>
#include <sys/time.h>
#include <string.h>
#include "log_manager.h"
#include "app_log.h"

int host_mounted, host_health_check, host_synced, host_leases, host_allocations, host_tasks;
int host_console, host_writes, host_syncs, host_fail_write, host_fail_sync, host_delay;
int host_fail_alloc, host_fail_task, host_fail_unlink;
struct host_task {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint32_t bits;
    void (*function)(void *);
    void *arg;
};
static _Thread_local TaskHandle_t current;
static int sink_level = ESP_LOG_INFO;
static struct timespec deadline(unsigned ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { ++t.tv_sec; t.tv_nsec -= 1000000000L; }
    return t;
}
void host_sleep(unsigned ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s)
{
    pthread_mutex_init(&s->mutex, NULL); pthread_cond_init(&s->cond, NULL); s->available = false; return s;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
{
    xSemaphoreCreateBinaryStatic(s); s->available = true; return s;
}
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    struct timespec until = deadline(ticks);
    pthread_mutex_lock(&s->mutex);
    while (!s->available && ticks) {
        if (pthread_cond_timedwait(&s->cond, &s->mutex, &until) == ETIMEDOUT) break;
    }
    bool ok = s->available;
    if (ok) s->available = false;
    pthread_mutex_unlock(&s->mutex); return ok;
}
void xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->mutex); s->available = true;
    pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->mutex);
}
static void *run(void *p) { current = p; current->function(current->arg); return NULL; }
int xTaskCreate(void (*function)(void *), const char *name, int stack, void *arg, int priority, TaskHandle_t *task)
{
    (void)name; (void)stack; (void)priority;
    if (__atomic_exchange_n(&host_fail_task, 0, __ATOMIC_RELAXED)) return 0;
    TaskHandle_t t = calloc(1, sizeof(*t)); assert(t);
    pthread_mutex_init(&t->mutex, NULL); pthread_cond_init(&t->cond, NULL);
    t->function = function; t->arg = arg; *task = t;
    __atomic_add_fetch(&host_tasks, 1, __ATOMIC_RELAXED);
    pthread_t thread; assert(!pthread_create(&thread, NULL, run, t)); pthread_detach(thread); return pdPASS;
}
void xTaskNotify(TaskHandle_t t, uint32_t bits, int action)
{
    (void)action; pthread_mutex_lock(&t->mutex); t->bits |= bits;
    pthread_cond_signal(&t->cond); pthread_mutex_unlock(&t->mutex);
}
void xTaskNotifyWait(uint32_t entry, uint32_t exit, uint32_t *bits, TickType_t ticks)
{
    (void)entry; struct timespec until = deadline(ticks);
    pthread_mutex_lock(&current->mutex);
    while (!current->bits) {
        if (pthread_cond_timedwait(&current->cond, &current->mutex, &until) == ETIMEDOUT) break;
    }
    *bits = current->bits; current->bits &= ~exit; pthread_mutex_unlock(&current->mutex);
}
void vTaskDelete(TaskHandle_t task)
{
    assert(!task); pthread_mutex_destroy(&current->mutex); pthread_cond_destroy(&current->cond);
    free(current); __atomic_sub_fetch(&host_tasks, 1, __ATOMIC_RELAXED); pthread_exit(NULL);
}
int64_t esp_timer_get_time(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
TickType_t xTaskGetTickCount(void) { return esp_timer_get_time() / 1000; }
bool xPortInIsrContext(void) { return false; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
uint32_t esp_random(void) { static uint32_t n; return ++n; }
void *heap_caps_malloc(size_t bytes, int caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (__atomic_exchange_n(&host_fail_alloc, 0, __ATOMIC_RELAXED)) return NULL;
    void *p = malloc(bytes);
    if (p) ++host_allocations;
    return p;
}
void heap_caps_free(void *p) { if (p) --host_allocations; free(p); }
bool esp_ptr_external_ram(const void *p) { return p != NULL; }
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
    (void)format;
    if (!strcmp(tag, "APP_LOG_SINK") && (int)level > __atomic_load_n(&sink_level, __ATOMIC_RELAXED)) return;
    __atomic_add_fetch(&host_console, 1, __ATOMIC_RELAXED);
}
void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    assert(!strcmp(tag, "APP_LOG_SINK")); __atomic_store_n(&sink_level, level, __ATOMIC_RELAXED);
}
bool sd_card_manager_is_mounted(void)
{
    return __atomic_load_n(&host_mounted, __ATOMIC_RELAXED) &&
           !__atomic_load_n(&host_health_check, __ATOMIC_RELAXED);
}
esp_err_t sd_card_manager_get_status(sd_card_manager_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    status->state = __atomic_load_n(&host_mounted, __ATOMIC_RELAXED)
        ? SD_CARD_MANAGER_STATE_READY
        : SD_CARD_MANAGER_STATE_UNAVAILABLE;
    return ESP_OK;
}
esp_err_t sd_card_manager_acquire(void)
{
    if (!sd_card_manager_is_mounted()) return ESP_ERR_INVALID_STATE;
    __atomic_add_fetch(&host_leases, 1, __ATOMIC_RELAXED); return ESP_OK;
}
void sd_card_manager_release(void) { assert(__atomic_sub_fetch(&host_leases, 1, __ATOMIC_RELAXED) >= 0); }
void sd_card_manager_report_io_error(esp_err_t error)
{
    APP_LOGW("SD_MANAGER", SD_IO_ERROR, "error=%d", error);
    (void)error; __atomic_store_n(&host_mounted, 0, __ATOMIC_RELAXED); log_manager_notify_environment_changed();
}
bool sd_card_manager_is_vfs_media_error(int error) { return error == EIO; }
bool time_manager_is_synced(void) { return __atomic_load_n(&host_synced, __ATOMIC_RELAXED); }
esp_err_t time_manager_get_local_time(struct tm *local)
{
    time_t t = time(NULL); return time_manager_is_synced() && localtime_r(&t, local) ? ESP_OK : ESP_FAIL;
}
size_t host_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    assert(current); /* Every production filesystem write must be writer-owned. */
    __atomic_add_fetch(&host_writes, 1, __ATOMIC_RELAXED);
    int delay = __atomic_load_n(&host_delay, __ATOMIC_RELAXED); if (delay) host_sleep(delay);
    if (__atomic_exchange_n(&host_fail_write, 0, __ATOMIC_RELAXED)) { errno = EIO; return 0; }
    return fwrite(data, size, count, file);
}
int host_fsync(int fd)
{
    assert(current); __atomic_add_fetch(&host_syncs, 1, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&host_fail_sync, 0, __ATOMIC_RELAXED)) { errno = EIO; return -1; }
    return _commit(fd);
}
int host_mkdir(const char *path, unsigned mode) { (void)mode; return _mkdir(path); }
int host_unlink(const char *path)
{
    assert(current);
    if (__atomic_exchange_n(&host_fail_unlink, 0, __ATOMIC_RELAXED)) { errno = EACCES; return -1; }
    return _unlink(path);
}