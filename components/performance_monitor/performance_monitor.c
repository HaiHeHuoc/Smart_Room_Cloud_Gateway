/* Includes ----------------------------------------------------------------- */
#include "performance_monitor.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Macros ------------------------------------------------------------------- */

#define PERF_MONITOR_TASK_NAME              "perf_monitor"
#define PERF_MONITOR_TASK_STACK_SIZE_BYTES  (6U * 1024U)
#define PERF_MONITOR_TASK_PRIORITY          2U

/*
 * CPU usage is measured over this time window.
 *
 * A longer window gives a more stable value and reduces monitoring overhead.
 */
#define PERF_MONITOR_PERIOD_MS              5000U

/*
 * Maximum number of FreeRTOS tasks captured in one snapshot.
 *
 * Increase this if the project creates more than 40 tasks.
 * Arrays are static, so they do not consume task stack.
 */
#define PERF_MONITOR_MAX_TASKS              40U

/*
 * App/Flash information does not change at runtime, so print it less often.
 *
 * 12 samples × 5 seconds = once every 60 seconds.
 */
#define PERF_MONITOR_FLASH_LOG_INTERVAL      12U

#define PERF_INTERNAL_RAM_CAPS \
    (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

#define PERF_DMA_RAM_CAPS \
    (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "PERF_MONITOR";

/* Type Definitions --------------------------------------------------------- */
typedef struct {
    uint32_t used_x10;
    uint32_t idle_x10;
} performance_monitor_cpu_result_t;

/* Static Variables --------------------------------------------------------- */
static TaskHandle_t s_monitor_task_handle = NULL;

/*
 * Keep task snapshots outside the task stack.
 *
 * Two snapshots are needed to calculate runtime differences over one period.
 */
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static TaskStatus_t s_start_snapshot[PERF_MONITOR_MAX_TASKS];
static TaskStatus_t s_end_snapshot[PERF_MONITOR_MAX_TASKS];
#endif

/* Function Prototypes ------------------------------------------------------ */
static bool performance_monitor_is_idle_task(
    const char *task_name);

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static const TaskStatus_t *performance_monitor_find_task(
    const TaskStatus_t *tasks,
    UBaseType_t task_count,
    TaskHandle_t task_handle);
static esp_err_t performance_monitor_measure_cpu(
    TickType_t measurement_ticks,
    performance_monitor_cpu_result_t *result);
#endif

static void performance_monitor_log_cpu(
    const performance_monitor_cpu_result_t *cpu);
static void performance_monitor_log_heap_region(
    const char *name,
    uint32_t capabilities);
static void performance_monitor_log_memory(void);
static void performance_monitor_log_app_flash(void);
static void performance_monitor_log_task_stack(void);
static void performance_monitor_task(void *argument);

/* Static Functions --------------------------------------------------------- */

static bool performance_monitor_is_idle_task(const char *task_name)
{
    if (task_name == NULL) {
        return false;
    }

    /*
     * ESP-IDF idle tasks are normally named IDLE0 and IDLE1.
     */
    return strncmp(task_name, "IDLE", 4U) == 0;
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

static const TaskStatus_t *performance_monitor_find_task(
    const TaskStatus_t *tasks,
    UBaseType_t task_count,
    TaskHandle_t task_handle)
{
    if ((tasks == NULL) || (task_handle == NULL)) {
        return NULL;
    }

    for (UBaseType_t index = 0U;
         index < task_count;
         ++index) {
        if (tasks[index].xHandle == task_handle) {
            return &tasks[index];
        }
    }

    return NULL;
}

static esp_err_t performance_monitor_measure_cpu(
    TickType_t measurement_ticks,
    performance_monitor_cpu_result_t *result)
{
    ESP_RETURN_ON_FALSE(
        result != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "CPU result pointer is NULL"
    );

    result->used_x10 = 0U;
    result->idle_x10 = 0U;

    const UBaseType_t current_task_count =
        uxTaskGetNumberOfTasks();

    ESP_RETURN_ON_FALSE(
        current_task_count <= PERF_MONITOR_MAX_TASKS,
        ESP_ERR_INVALID_SIZE,
        TAG,
        "Too many tasks: current=%u, maximum=%u",
        (unsigned int)current_task_count,
        (unsigned int)PERF_MONITOR_MAX_TASKS
    );

    configRUN_TIME_COUNTER_TYPE start_total_runtime = 0;
    configRUN_TIME_COUNTER_TYPE end_total_runtime = 0;

    const UBaseType_t start_task_count =
        uxTaskGetSystemState(
            s_start_snapshot,
            PERF_MONITOR_MAX_TASKS,
            &start_total_runtime
        );

    ESP_RETURN_ON_FALSE(
        start_task_count > 0U,
        ESP_FAIL,
        TAG,
        "Failed to capture initial task snapshot"
    );

    /*
     * CPU usage is calculated from runtime differences between two snapshots.
     */
    vTaskDelay(measurement_ticks);

    const UBaseType_t end_task_count =
        uxTaskGetSystemState(
            s_end_snapshot,
            PERF_MONITOR_MAX_TASKS,
            &end_total_runtime
        );

    ESP_RETURN_ON_FALSE(
        end_task_count > 0U,
        ESP_FAIL,
        TAG,
        "Failed to capture final task snapshot"
    );

    const configRUN_TIME_COUNTER_TYPE elapsed_native =
        end_total_runtime - start_total_runtime;

    const uint64_t elapsed_runtime =
        (uint64_t)elapsed_native;

    ESP_RETURN_ON_FALSE(
        elapsed_runtime > 0U,
        ESP_ERR_INVALID_STATE,
        TAG,
        "CPU measurement window is too short"
    );

    uint64_t idle_runtime = 0U;

    for (UBaseType_t index = 0U;
         index < start_task_count;
         ++index) {

        const TaskStatus_t *start_task =
            &s_start_snapshot[index];

        const TaskStatus_t *end_task =
            performance_monitor_find_task(
                s_end_snapshot,
                end_task_count,
                start_task->xHandle
            );

        /*
         * The task might have been deleted during the measurement window.
         */
        if (end_task == NULL) {
            continue;
        }

        const configRUN_TIME_COUNTER_TYPE task_delta_native =
            end_task->ulRunTimeCounter -
            start_task->ulRunTimeCounter;

        const uint64_t task_delta =
            (uint64_t)task_delta_native;

        if (performance_monitor_is_idle_task(
                start_task->pcTaskName)) {
            idle_runtime += task_delta;
        }
    }

    /*
     * In dual-core mode, total CPU capacity over a wall-time interval is:
     *
     *     elapsed_runtime × number_of_cores
     *
     * The official ESP-IDF runtime-statistics example uses the same
     * core-count adjustment.
     */
    const uint64_t total_cpu_capacity =
        elapsed_runtime *
        (uint64_t)CONFIG_FREERTOS_NUMBER_OF_CORES;

    ESP_RETURN_ON_FALSE(
        total_cpu_capacity > 0U,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Invalid CPU capacity"
    );

    if (idle_runtime > total_cpu_capacity) {
        /*
         * Protect against snapshot inconsistency or counter rollover.
         */
        idle_runtime = total_cpu_capacity;
    }

    const uint64_t busy_runtime =
        total_cpu_capacity - idle_runtime;

    /*
     * Percentage × 10:
     *
     *     653 means 65.3%
     */
    result->used_x10 =
        (uint32_t)(
            (busy_runtime * 1000ULL) /
            total_cpu_capacity
        );

    result->idle_x10 =
        (uint32_t)(
            (idle_runtime * 1000ULL) /
            total_cpu_capacity
        );

    return ESP_OK;
}

#endif /* CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

static void performance_monitor_log_cpu(
    const performance_monitor_cpu_result_t *cpu)
{
    if (cpu == NULL) {
        return;
    }

    ESP_LOGI(
        TAG,
        "[CPU] used=%u.%u%%, idle=%u.%u%%, cores=%u",
        (unsigned int)(cpu->used_x10 / 10U),
        (unsigned int)(cpu->used_x10 % 10U),
        (unsigned int)(cpu->idle_x10 / 10U),
        (unsigned int)(cpu->idle_x10 % 10U),
        (unsigned int)CONFIG_FREERTOS_NUMBER_OF_CORES
    );
}

static void performance_monitor_log_heap_region(
    const char *name,
    uint32_t caps)
{
    const size_t total =
        heap_caps_get_total_size(caps);

    const size_t free_now =
        heap_caps_get_free_size(caps);

    const size_t minimum_free =
        heap_caps_get_minimum_free_size(caps);

    const size_t largest_block =
        heap_caps_get_largest_free_block(caps);

    if (total == 0U) {
        ESP_LOGI(
            TAG,
            "[RAM:%s] not available",
            name
        );

        return;
    }

    const size_t used =
        total >= free_now
            ? total - free_now
            : 0U;

    const uint32_t used_percent_x10 =
        total > 0U
            ? (uint32_t)(
                ((uint64_t)used * 1000ULL) /
                total
              )
            : 0U;

    ESP_LOGI(
        TAG,
        "[RAM:%s] total=%u, used=%u (%u.%u%%), "
        "free=%u, minimum=%u, largest=%u bytes",
        name,
        (unsigned int)total,
        (unsigned int)used,
        (unsigned int)(used_percent_x10 / 10U),
        (unsigned int)(used_percent_x10 % 10U),
        (unsigned int)free_now,
        (unsigned int)minimum_free,
        (unsigned int)largest_block
    );
}

static void performance_monitor_log_memory(void)
{
    performance_monitor_log_heap_region(
        "INTERNAL",
        PERF_INTERNAL_RAM_CAPS
    );

    performance_monitor_log_heap_region(
        "PSRAM",
        MALLOC_CAP_SPIRAM
    );

    performance_monitor_log_heap_region(
        "DMA",
        PERF_DMA_RAM_CAPS
    );
}

static void performance_monitor_log_app_flash(void)
{
    const esp_partition_t *running_partition =
        esp_ota_get_running_partition();

    if (running_partition == NULL) {
        ESP_LOGW(
            TAG,
            "[FLASH] Cannot obtain running app partition"
        );

        return;
    }

    const esp_partition_pos_t partition_position = {
        .offset = running_partition->address,
        .size = running_partition->size,
    };

    esp_image_metadata_t metadata = {0};

    const esp_err_t metadata_result =
        esp_image_get_metadata(
            &partition_position,
            &metadata
        );

    if (metadata_result != ESP_OK) {
        ESP_LOGW(
            TAG,
            "[FLASH] Cannot read app metadata: %s",
            esp_err_to_name(metadata_result)
        );

        ESP_LOGI(
            TAG,
            "[FLASH] partition=%s, capacity=%u bytes",
            running_partition->label,
            (unsigned int)running_partition->size
        );

        return;
    }

    const uint32_t image_size =
        metadata.image_len;

    const uint32_t free_in_partition =
        running_partition->size >= image_size
            ? running_partition->size - image_size
            : 0U;

    const uint32_t used_percent_x10 =
        running_partition->size > 0U
            ? (uint32_t)(
                ((uint64_t)image_size * 1000ULL) /
                running_partition->size
              )
            : 0U;

    ESP_LOGI(
        TAG,
        "[FLASH] partition=%s, image=%u, capacity=%u, "
        "free=%u bytes, used=%u.%u%%",
        running_partition->label,
        (unsigned int)image_size,
        (unsigned int)running_partition->size,
        (unsigned int)free_in_partition,
        (unsigned int)(used_percent_x10 / 10U),
        (unsigned int)(used_percent_x10 % 10U)
    );
}

static void performance_monitor_log_task_stack(void)
{
    const UBaseType_t minimum_stack_remaining =
        uxTaskGetStackHighWaterMark(NULL);

    ESP_LOGI(
        TAG,
        "[STACK] task=%s, minimum remaining=%u bytes",
        pcTaskGetName(NULL),
        (unsigned int)minimum_stack_remaining
    );
}

static void performance_monitor_task(void *argument)
{
    (void)argument;

    uint32_t sample_index = 0U;

    ESP_LOGI(
        TAG,
        "Performance monitor started: period=%u ms",
        (unsigned int)PERF_MONITOR_PERIOD_MS
    );

    /*
     * Print firmware partition information immediately.
     */
    performance_monitor_log_app_flash();

    while (true) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

        performance_monitor_cpu_result_t cpu = {0};

        const esp_err_t cpu_result =
            performance_monitor_measure_cpu(
                pdMS_TO_TICKS(PERF_MONITOR_PERIOD_MS),
                &cpu
            );

        if (cpu_result == ESP_OK) {
            performance_monitor_log_cpu(&cpu);
        }
        else {
            ESP_LOGE(
                TAG,
                "CPU measurement failed: %s",
                esp_err_to_name(cpu_result)
            );

            /*
             * Avoid a fast error loop.
             */
            vTaskDelay(
                pdMS_TO_TICKS(PERF_MONITOR_PERIOD_MS)
            );
        }

#else

        ESP_LOGE(
            TAG,
            "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is disabled"
        );

        vTaskDelay(
            pdMS_TO_TICKS(PERF_MONITOR_PERIOD_MS)
        );

#endif

        performance_monitor_log_memory();
        performance_monitor_log_task_stack();

        ++sample_index;

        if ((sample_index %
             PERF_MONITOR_FLASH_LOG_INTERVAL) == 0U) {
            performance_monitor_log_app_flash();
        }
    }
}

/* Functions ---------------------------------------------------------------- */

esp_err_t performance_monitor_start(void)
{
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

    ESP_LOGE(
        TAG,
        "Enable configGENERATE_RUN_TIME_STATS in menuconfig"
    );

    return ESP_ERR_NOT_SUPPORTED;

#else

    ESP_RETURN_ON_FALSE(
        s_monitor_task_handle == NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Performance monitor is already running"
    );

    const BaseType_t task_result =
        xTaskCreate(
            performance_monitor_task,
            PERF_MONITOR_TASK_NAME,
            PERF_MONITOR_TASK_STACK_SIZE_BYTES,
            NULL,
            PERF_MONITOR_TASK_PRIORITY,
            &s_monitor_task_handle
        );

    if (task_result != pdPASS) {
        s_monitor_task_handle = NULL;

        ESP_LOGE(
            TAG,
            "Failed to create performance monitor task"
        );

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;

#endif
}
