/* Includes ----------------------------------------------------------------- */
#include "performance_monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"

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
 * Print the complete task table on the first report and then every 60 seconds.
 *
 * 12 reports x 5 seconds = 60 seconds.
 */
#define PERF_MONITOR_FULL_TASK_INTERVAL     12U

#define PERF_MONITOR_STACK_WARNING_BYTES    1024U

#define PERF_INTERNAL_RAM_CAPS \
    (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

#define PERF_PSRAM_CAPS \
    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#define PERF_DMA_RAM_CAPS \
    (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

/* Constants ---------------------------------------------------------------- */

static const char *const TAG = "PERF_MONITOR";

/* Type Definitions --------------------------------------------------------- */

typedef struct {
    uint32_t used_x10;
    uint32_t idle_x10;
    uint32_t core_used_x10[CONFIG_FREERTOS_NUMBER_OF_CORES];
    bool core_valid[CONFIG_FREERTOS_NUMBER_OF_CORES];
    uint64_t elapsed_runtime;
    uint64_t total_cpu_capacity;
    UBaseType_t start_task_count;
    UBaseType_t end_task_count;
} performance_monitor_cpu_result_t;

typedef struct {
    UBaseType_t running;
    UBaseType_t ready;
    UBaseType_t blocked;
    UBaseType_t suspended;
    UBaseType_t deleted;
    UBaseType_t invalid;
} performance_monitor_task_state_counts_t;

/* Static Variables --------------------------------------------------------- */

static TaskHandle_t s_monitor_task_handle = NULL;

/*
 * Keep task snapshots outside the task stack.
 *
 * Two snapshots are needed to calculate runtime differences over one period.
 */
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
EXT_RAM_BSS_ATTR static TaskStatus_t s_start_snapshot[PERF_MONITOR_MAX_TASKS];
EXT_RAM_BSS_ATTR static TaskStatus_t s_end_snapshot[PERF_MONITOR_MAX_TASKS];
#endif

/* Function Prototypes ------------------------------------------------------ */

static bool performance_monitor_is_idle_task(
    const char *task_name);

static int32_t performance_monitor_get_idle_core_index(
    const char *task_name);

static const char *performance_monitor_reset_reason_to_string(
    esp_reset_reason_t reason);

static const char *performance_monitor_task_state_to_string(
    eTaskState state);

static const char *performance_monitor_stack_location_to_string(
    const TaskStatus_t *task);

static void performance_monitor_log_boot_information(void);

static void performance_monitor_log_report_header(
    uint32_t report_index);

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static const TaskStatus_t *performance_monitor_find_task(
    const TaskStatus_t *tasks,
    UBaseType_t task_count,
    TaskHandle_t task_handle);

static esp_err_t performance_monitor_measure_cpu(
    TickType_t measurement_ticks,
    performance_monitor_cpu_result_t *result);

static uint32_t performance_monitor_get_task_cpu_x10(
    const TaskStatus_t *start_task,
    const TaskStatus_t *end_task,
    uint64_t total_cpu_capacity);
#endif

static void performance_monitor_log_cpu(
    uint32_t report_index,
    const performance_monitor_cpu_result_t *cpu);

static void performance_monitor_log_heap_region(
    uint32_t report_index,
    const char *name,
    uint32_t capabilities);

static void performance_monitor_log_memory(
    uint32_t report_index);

static void performance_monitor_log_app_flash(void);

static void performance_monitor_log_task_summary(
    uint32_t report_index,
    const TaskStatus_t *tasks,
    UBaseType_t task_count);

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static void performance_monitor_log_task_table(
    uint32_t report_index,
    const performance_monitor_cpu_result_t *cpu);
#endif

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

static int32_t performance_monitor_get_idle_core_index(
    const char *task_name)
{
    if (!performance_monitor_is_idle_task(task_name)) {
        return -1;
    }

    const char core_character = task_name[4];

    if ((core_character < '0') ||
        (core_character > '9')) {
        return -1;
    }

    const int32_t core_index =
        (int32_t)(core_character - '0');

    if ((core_index < 0) ||
        (core_index >= CONFIG_FREERTOS_NUMBER_OF_CORES)) {
        return -1;
    }

    return core_index;
}

static const char *performance_monitor_reset_reason_to_string(
    esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "POWER_ON";

        case ESP_RST_EXT:
            return "EXTERNAL_PIN";

        case ESP_RST_SW:
            return "SOFTWARE";

        case ESP_RST_PANIC:
            return "PANIC";

        case ESP_RST_INT_WDT:
            return "INTERRUPT_WDT";

        case ESP_RST_TASK_WDT:
            return "TASK_WDT";

        case ESP_RST_WDT:
            return "OTHER_WDT";

        case ESP_RST_DEEPSLEEP:
            return "DEEP_SLEEP";

        case ESP_RST_BROWNOUT:
            return "BROWNOUT";

        case ESP_RST_SDIO:
            return "SDIO";

        case ESP_RST_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

static const char *performance_monitor_task_state_to_string(
    eTaskState state)
{
    switch (state) {
        case eRunning:
            return "RUNNING";

        case eReady:
            return "READY";

        case eBlocked:
            return "BLOCKED";

        case eSuspended:
            return "SUSPENDED";

        case eDeleted:
            return "DELETED";

        case eInvalid:
        default:
            return "INVALID";
    }
}

static const char *performance_monitor_stack_location_to_string(
    const TaskStatus_t *task)
{
    if ((task == NULL) ||
        (task->pxStackBase == NULL)) {
        return "UNKNOWN";
    }

    if (esp_ptr_external_ram(task->pxStackBase)) {
        return "PSRAM";
    }

    if (esp_ptr_internal(task->pxStackBase)) {
        return "INTERNAL";
    }

    return "OTHER";
}

static void performance_monitor_log_boot_information(void)
{
    esp_chip_info_t chip_info = {0};
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0U;
    const esp_err_t flash_result =
        esp_flash_get_size(
            NULL,
            &flash_size);

    const size_t psram_size =
        esp_psram_get_size();

    ESP_LOGI(
        TAG,
        "[REPORT:000000][CHIP] target=%s, revision=%u, cores=%u, "
        "cpu_config=%u MHz, idf=%s, reset=%s",
        CONFIG_IDF_TARGET,
        (unsigned int)chip_info.revision,
        (unsigned int)chip_info.cores,
        (unsigned int)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        esp_get_idf_version(),
        performance_monitor_reset_reason_to_string(
            esp_reset_reason())
    );

    if (flash_result == ESP_OK) {
        ESP_LOGI(
            TAG,
            "[REPORT:000000][MEMORY_HW] flash=%u bytes, psram=%u bytes",
            (unsigned int)flash_size,
            (unsigned int)psram_size
        );
    }
    else {
        ESP_LOGW(
            TAG,
            "[REPORT:000000][MEMORY_HW] flash=unknown (%s), psram=%u bytes",
            esp_err_to_name(flash_result),
            (unsigned int)psram_size
        );
    }
}

static void performance_monitor_log_report_header(
    uint32_t report_index)
{
    const uint64_t uptime_seconds =
        (uint64_t)esp_timer_get_time() /
        1000000ULL;

    ESP_LOGI(
        TAG,
        "[REPORT:%06u][SYSTEM] uptime=%llu s, period=%u ms",
        (unsigned int)report_index,
        (unsigned long long)uptime_seconds,
        (unsigned int)PERF_MONITOR_PERIOD_MS
    );
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

static const TaskStatus_t *performance_monitor_find_task(
    const TaskStatus_t *tasks,
    UBaseType_t task_count,
    TaskHandle_t task_handle)
{
    if ((tasks == NULL) ||
        (task_handle == NULL)) {
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

    memset(result, 0, sizeof(*result));

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
    uint64_t core_idle_runtime[
        CONFIG_FREERTOS_NUMBER_OF_CORES] = {0};

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

        if (!performance_monitor_is_idle_task(
                start_task->pcTaskName)) {
            continue;
        }

        idle_runtime += task_delta;

        const int32_t idle_core_index =
            performance_monitor_get_idle_core_index(
                start_task->pcTaskName);

        if (idle_core_index >= 0) {
            core_idle_runtime[idle_core_index] +=
                task_delta;

            result->core_valid[idle_core_index] =
                true;
        }
    }

    /*
     * In dual-core mode, total CPU capacity over a wall-time interval is:
     *
     *     elapsed_runtime x number_of_cores
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

    for (UBaseType_t core_index = 0U;
         core_index < CONFIG_FREERTOS_NUMBER_OF_CORES;
         ++core_index) {

        if (!result->core_valid[core_index]) {
            continue;
        }

        if (core_idle_runtime[core_index] >
            elapsed_runtime) {
            core_idle_runtime[core_index] =
                elapsed_runtime;
        }

        const uint64_t core_busy_runtime =
            elapsed_runtime -
            core_idle_runtime[core_index];

        result->core_used_x10[core_index] =
            (uint32_t)(
                (core_busy_runtime * 1000ULL) /
                elapsed_runtime
            );
    }

    result->elapsed_runtime = elapsed_runtime;
    result->total_cpu_capacity =
        total_cpu_capacity;
    result->start_task_count =
        start_task_count;
    result->end_task_count =
        end_task_count;

    return ESP_OK;
}

static uint32_t performance_monitor_get_task_cpu_x10(
    const TaskStatus_t *start_task,
    const TaskStatus_t *end_task,
    uint64_t total_cpu_capacity)
{
    if ((start_task == NULL) ||
        (end_task == NULL) ||
        (total_cpu_capacity == 0U)) {
        return 0U;
    }

    const configRUN_TIME_COUNTER_TYPE delta_native =
        end_task->ulRunTimeCounter -
        start_task->ulRunTimeCounter;

    const uint64_t task_delta =
        (uint64_t)delta_native;

    return (uint32_t)(
        (task_delta * 1000ULL) /
        total_cpu_capacity
    );
}

#endif /* CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

static void performance_monitor_log_cpu(
    uint32_t report_index,
    const performance_monitor_cpu_result_t *cpu)
{
    if (cpu == NULL) {
        return;
    }

    ESP_LOGI(
        TAG,
        "[REPORT:%06u][CPU] used=%u.%u%%, idle=%u.%u%%, cores=%u",
        (unsigned int)report_index,
        (unsigned int)(cpu->used_x10 / 10U),
        (unsigned int)(cpu->used_x10 % 10U),
        (unsigned int)(cpu->idle_x10 / 10U),
        (unsigned int)(cpu->idle_x10 % 10U),
        (unsigned int)CONFIG_FREERTOS_NUMBER_OF_CORES
    );

    for (UBaseType_t core_index = 0U;
         core_index < CONFIG_FREERTOS_NUMBER_OF_CORES;
         ++core_index) {

        if (!cpu->core_valid[core_index]) {
            continue;
        }

        ESP_LOGI(
            TAG,
            "[REPORT:%06u][CPU:CORE%u] used=%u.%u%%",
            (unsigned int)report_index,
            (unsigned int)core_index,
            (unsigned int)(
                cpu->core_used_x10[core_index] / 10U),
            (unsigned int)(
                cpu->core_used_x10[core_index] % 10U)
        );
    }
}

static void performance_monitor_log_heap_region(
    uint32_t report_index,
    const char *name,
    uint32_t capabilities)
{
    multi_heap_info_t heap_info = {0};

    heap_caps_get_info(
        &heap_info,
        capabilities);

    const size_t total =
        heap_caps_get_total_size(capabilities);

    const size_t free_now =
        heap_info.total_free_bytes;

    const size_t minimum_free =
        heap_info.minimum_free_bytes;

    const size_t largest_block =
        heap_info.largest_free_block;

    if (total == 0U) {
        ESP_LOGI(
            TAG,
            "[REPORT:%06u][RAM:%s] not available",
            (unsigned int)report_index,
            name
        );

        return;
    }

    const size_t used =
        total >= free_now
            ? total - free_now
            : 0U;

    const uint32_t used_percent_x10 =
        (uint32_t)(
            ((uint64_t)used * 1000ULL) /
            total
        );

    const uint32_t fragmentation_x10 =
        free_now > 0U
            ? (uint32_t)(
                ((uint64_t)(
                    free_now - largest_block) *
                 1000ULL) /
                free_now
              )
            : 0U;

    ESP_LOGI(
        TAG,
        "[REPORT:%06u][RAM:%s] total=%u, used=%u (%u.%u%%), "
        "free=%u, minimum=%u, largest=%u, frag_est=%u.%u%%, "
        "alloc_blocks=%u, free_blocks=%u",
        (unsigned int)report_index,
        name,
        (unsigned int)total,
        (unsigned int)used,
        (unsigned int)(used_percent_x10 / 10U),
        (unsigned int)(used_percent_x10 % 10U),
        (unsigned int)free_now,
        (unsigned int)minimum_free,
        (unsigned int)largest_block,
        (unsigned int)(fragmentation_x10 / 10U),
        (unsigned int)(fragmentation_x10 % 10U),
        (unsigned int)heap_info.allocated_blocks,
        (unsigned int)heap_info.free_blocks
    );
}

static void performance_monitor_log_memory(
    uint32_t report_index)
{
    performance_monitor_log_heap_region(
        report_index,
        "INTERNAL",
        PERF_INTERNAL_RAM_CAPS
    );

    performance_monitor_log_heap_region(
        report_index,
        "PSRAM",
        PERF_PSRAM_CAPS
    );

    performance_monitor_log_heap_region(
        report_index,
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
            "[REPORT:000000][FLASH] Cannot obtain running app partition"
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
            "[REPORT:000000][FLASH] Cannot read app metadata: %s",
            esp_err_to_name(metadata_result)
        );

        ESP_LOGI(
            TAG,
            "[REPORT:000000][FLASH] partition=%s, capacity=%u bytes",
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
        "[REPORT:000000][FLASH] partition=%s, image=%u, capacity=%u, "
        "free=%u bytes, used=%u.%u%%",
        running_partition->label,
        (unsigned int)image_size,
        (unsigned int)running_partition->size,
        (unsigned int)free_in_partition,
        (unsigned int)(used_percent_x10 / 10U),
        (unsigned int)(used_percent_x10 % 10U)
    );
}

static void performance_monitor_log_task_summary(
    uint32_t report_index,
    const TaskStatus_t *tasks,
    UBaseType_t task_count)
{
    if ((tasks == NULL) ||
        (task_count == 0U)) {
        ESP_LOGW(
            TAG,
            "[REPORT:%06u][TASKS] no task snapshot available",
            (unsigned int)report_index
        );

        return;
    }

    performance_monitor_task_state_counts_t counts = {0};

    for (UBaseType_t index = 0U;
         index < task_count;
         ++index) {
        switch (tasks[index].eCurrentState) {
            case eRunning:
                ++counts.running;
                break;

            case eReady:
                ++counts.ready;
                break;

            case eBlocked:
                ++counts.blocked;
                break;

            case eSuspended:
                ++counts.suspended;
                break;

            case eDeleted:
                ++counts.deleted;
                break;

            case eInvalid:
            default:
                ++counts.invalid;
                break;
        }
    }

    ESP_LOGI(
        TAG,
        "[REPORT:%06u][TASKS] total=%u, running=%u, ready=%u, "
        "blocked=%u, suspended=%u, deleted=%u, invalid=%u",
        (unsigned int)report_index,
        (unsigned int)task_count,
        (unsigned int)counts.running,
        (unsigned int)counts.ready,
        (unsigned int)counts.blocked,
        (unsigned int)counts.suspended,
        (unsigned int)counts.deleted,
        (unsigned int)counts.invalid
    );
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

static void performance_monitor_log_task_table(
    uint32_t report_index,
    const performance_monitor_cpu_result_t *cpu)
{
    if ((cpu == NULL) ||
        (cpu->end_task_count == 0U)) {
        return;
    }

    ESP_LOGI(
        TAG,
        "[REPORT:%06u][TASK_TABLE] entries=%u, "
        "stack_min is the lowest remaining stack in bytes",
        (unsigned int)report_index,
        (unsigned int)cpu->end_task_count
    );

    for (UBaseType_t index = 0U;
         index < cpu->end_task_count;
         ++index) {

        const TaskStatus_t *end_task =
            &s_end_snapshot[index];

        const TaskStatus_t *start_task =
            performance_monitor_find_task(
                s_start_snapshot,
                cpu->start_task_count,
                end_task->xHandle
            );

        const uint32_t task_cpu_x10 =
            performance_monitor_get_task_cpu_x10(
                start_task,
                end_task,
                cpu->total_cpu_capacity
            );

        const UBaseType_t stack_minimum =
            end_task->usStackHighWaterMark;

        ESP_LOGI(
            TAG,
            "[REPORT:%06u][TASK] id=%u, name=%s, state=%s, "
            "priority=%u, cpu=%u.%u%%, stack_min=%u bytes, "
            "stack_location=%s",
            (unsigned int)report_index,
            (unsigned int)end_task->xTaskNumber,
            end_task->pcTaskName,
            performance_monitor_task_state_to_string(
                end_task->eCurrentState),
            (unsigned int)end_task->uxCurrentPriority,
            (unsigned int)(task_cpu_x10 / 10U),
            (unsigned int)(task_cpu_x10 % 10U),
            (unsigned int)stack_minimum,
            performance_monitor_stack_location_to_string(
                end_task)
        );

        if (stack_minimum <
            PERF_MONITOR_STACK_WARNING_BYTES) {
            ESP_LOGW(
                TAG,
                "[REPORT:%06u][STACK_WARNING] task=%s, "
                "minimum_remaining=%u bytes",
                (unsigned int)report_index,
                end_task->pcTaskName,
                (unsigned int)stack_minimum
            );
        }
    }
}

#endif /* CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS */

static void performance_monitor_task(void *argument)
{
    (void)argument;

    uint32_t report_index = 0U;

    ESP_LOGI(
        TAG,
        "Performance monitor started: period=%u ms, "
        "full_task_interval=%u reports",
        (unsigned int)PERF_MONITOR_PERIOD_MS,
        (unsigned int)PERF_MONITOR_FULL_TASK_INTERVAL
    );

    /*
     * Static chip and firmware information is printed only once.
     */
    performance_monitor_log_boot_information();
    performance_monitor_log_app_flash();

    while (true) {
        performance_monitor_cpu_result_t cpu = {0};

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

        const esp_err_t cpu_result =
            performance_monitor_measure_cpu(
                pdMS_TO_TICKS(PERF_MONITOR_PERIOD_MS),
                &cpu
            );

        ++report_index;

        performance_monitor_log_report_header(
            report_index);

        if (cpu_result == ESP_OK) {
            performance_monitor_log_cpu(
                report_index,
                &cpu);
        }
        else {
            ESP_LOGE(
                TAG,
                "[REPORT:%06u][CPU] measurement failed: %s",
                (unsigned int)report_index,
                esp_err_to_name(cpu_result)
            );

            /*
             * Avoid a fast error loop when a snapshot cannot be captured.
             */
            vTaskDelay(
                pdMS_TO_TICKS(PERF_MONITOR_PERIOD_MS)
            );
        }

#else

        ++report_index;

        performance_monitor_log_report_header(
            report_index);

        ESP_LOGE(
            TAG,
            "[REPORT:%06u][CPU] "
            "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is disabled",
            (unsigned int)report_index
        );

        vTaskDelay(
            pdMS_TO_TICKS(PERF_MONITOR_PERIOD_MS)
        );

#endif

        performance_monitor_log_memory(
            report_index);

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

        if (cpu.end_task_count > 0U) {
            performance_monitor_log_task_summary(
                report_index,
                s_end_snapshot,
                cpu.end_task_count
            );

            if ((report_index == 1U) ||
                ((report_index %
                  PERF_MONITOR_FULL_TASK_INTERVAL) == 0U)) {
                performance_monitor_log_task_table(
                    report_index,
                    &cpu
                );
            }
        }

#endif
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
