#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
static inline struct tm *localtime_r(const time_t *value, struct tm *result)
{
    return localtime_s(result, value) == 0 ? result : NULL;
}

#ifndef CONFIG_LOG_MANAGER_ENABLE
#define CONFIG_LOG_MANAGER_ENABLE 1
#endif
#define CONFIG_LOG_MANAGER_BUFFER_KIB 16
#define CONFIG_LOG_MANAGER_RECORD_BYTES 512
#define CONFIG_LOG_MANAGER_WRITE_BYTES 1024
#define CONFIG_LOG_MANAGER_WRITE_MS 100
#define CONFIG_LOG_MANAGER_SYNC_MS 250
#define CONFIG_LOG_MANAGER_ROTATE_KIB 4
#define CONFIG_LOG_MANAGER_RETAIN_MIB 1
#define CONFIG_LOG_MANAGER_CONSOLE_LEVEL 3
#define CONFIG_LOG_MANAGER_STORAGE_LEVEL 4
#define CONFIG_LOG_DYNAMIC_LEVEL_CONTROL 1
#define CONFIG_LOG_TAG_LEVEL_IMPL_NONE 0
#define SD_MOUNT_POINT "sd"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_NOT_SUPPORTED 5
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define taskYIELD() sched_yield()
#define eSetBits 1
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool available;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef struct host_task *TaskHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s);
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
void xSemaphoreGive(SemaphoreHandle_t s);
int xTaskCreate(void (*function)(void *), const char *name, int stack, void *arg, int priority, TaskHandle_t *task);
void xTaskNotify(TaskHandle_t task, uint32_t bits, int action);
void xTaskNotifyWait(uint32_t entry, uint32_t exit, uint32_t *bits, TickType_t ticks);
void vTaskDelete(TaskHandle_t task);
TickType_t xTaskGetTickCount(void);
bool xPortInIsrContext(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
void *heap_caps_malloc(size_t bytes, int caps);
void heap_caps_free(void *pointer);
bool esp_ptr_external_ram(const void *pointer);
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...);
void esp_log_level_set(const char *tag, esp_log_level_t level);
#define ESP_LOGW(tag, ...) esp_log_write(ESP_LOG_WARN, tag, __VA_ARGS__)

typedef enum {
    SD_CARD_MANAGER_STATE_UNINITIALIZED = 0,
    SD_CARD_MANAGER_STATE_INITIALIZING,
    SD_CARD_MANAGER_STATE_MOUNTING,
    SD_CARD_MANAGER_STATE_RETRY_WAIT,
    SD_CARD_MANAGER_STATE_READY,
    SD_CARD_MANAGER_STATE_RECOVERING,
    SD_CARD_MANAGER_STATE_UNAVAILABLE,
} sd_card_manager_state_t;

typedef struct {
    sd_card_manager_state_t state;
} sd_card_manager_status_t;

bool sd_card_manager_is_mounted(void);
esp_err_t sd_card_manager_get_status(sd_card_manager_status_t *status);
esp_err_t sd_card_manager_acquire(void);
void sd_card_manager_release(void);
void sd_card_manager_report_io_error(esp_err_t error);
bool sd_card_manager_is_vfs_media_error(int error);
bool time_manager_is_synced(void);
esp_err_t time_manager_get_local_time(struct tm *local);

extern int host_mounted, host_health_check, host_synced, host_leases, host_allocations, host_tasks;
extern int host_console, host_writes, host_syncs, host_fail_write, host_fail_sync, host_delay;
extern int host_fail_alloc, host_fail_task, host_fail_unlink;
void host_sleep(unsigned ms);
size_t host_fwrite(const void *data, size_t size, size_t count, FILE *file);
int host_fsync(int fd);
int host_mkdir(const char *path, unsigned mode);
int host_unlink(const char *path);