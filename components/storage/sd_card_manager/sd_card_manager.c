/* Includes ----------------------------------------------------------------- */
#include "sd_card_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

/* Macros ------------------------------------------------------------------- */
#define SD_CARD_MANAGER_TASK_STACK_SIZE_BYTES             4096U
#define SD_CARD_MANAGER_TASK_PRIORITY                         3U
#define SD_CARD_MANAGER_INITIAL_SETTLE_DELAY_MS            1000U
#define SD_CARD_MANAGER_INITIAL_RETRY_DELAY_MS             2000U
#define SD_CARD_MANAGER_INITIAL_RECOVERY_TIMEOUT_MS       90000U
#define SD_CARD_MANAGER_BACKGROUND_RETRY_DELAY_MS          2000U
#define SD_CARD_MANAGER_HEALTH_CHECK_INTERVAL_MS           5000U
#define SD_CARD_MANAGER_DRAIN_WAIT_MS                       500U
#define SD_CARD_MANAGER_BACKGROUND_FAILURE_LOG_PERIOD         15U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "sd_card_manager";

/* Static Variables --------------------------------------------------------- */
/* Only the recovery task mutates SPI, card, and VFS ownership below. */
static sdmmc_card_t *s_card = NULL;
static bool s_vfs_mounted = false;
static bool s_spi_bus_initialized = false;

/* Scalar state is shared with VFS consumers and guarded by this short lock. */
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized = false;
static bool s_started = false;
static bool s_health_check_in_progress = false;
static TaskHandle_t s_recovery_task_handle = NULL;
static sd_card_manager_status_t s_status = {
    .state = SD_CARD_MANAGER_STATE_UNINITIALIZED,
    .last_error = ESP_OK,
};

/* Function Prototypes ------------------------------------------------------ */
static esp_err_t sd_card_manager_init_spi_bus(void);
static esp_err_t sd_card_manager_release_spi_bus(void);
static esp_err_t sd_card_manager_mount_filesystem(void);
static esp_err_t sd_card_manager_mount_once(void);
static esp_err_t sd_card_manager_unmount_for_recovery(void);
static esp_err_t sd_card_manager_list_files_recursive_internal(
    const char *dir_path,
    uint8_t current_depth,
    uint8_t max_depth);
static void sd_card_manager_recovery_task(void *parameter);
static void sd_card_manager_notify_recovery_task(void);
static void sd_card_manager_mark_mount_attempt(void);
static void sd_card_manager_mark_mount_failure(
    esp_err_t error,
    bool in_initial_recovery_window);
static void sd_card_manager_mark_initial_timeout(void);
static void sd_card_manager_mark_ready(bool initial_recovery_timed_out);
static bool sd_card_manager_initial_deadline_expired(TickType_t initial_start);
static TickType_t sd_card_manager_initial_retry_wait_ticks(
    TickType_t initial_start);
static sdmmc_card_t *sd_card_manager_begin_idle_health_check(void);
static bool sd_card_manager_finish_idle_health_check(esp_err_t result);

/* Static Functions --------------------------------------------------------- */
/**
 * @brief Initialize the SPI bus used by the SD card.
 *
 * Recovery owns this call. Failed mount attempts release the bus again, so a
 * later attempt starts from a known component-owned state.
 */
static esp_err_t sd_card_manager_init_spi_bus(void)
{
    if (s_spi_bus_initialized)
    {
        return ESP_OK;
    }

    const spi_bus_config_t bus_config = {
        .miso_io_num = SD_GPIO_MISO,
        .mosi_io_num = SD_GPIO_MOSI,
        .sclk_io_num = SD_GPIO_SCLK,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };

    const esp_err_t result = spi_bus_initialize(
        SD_SPI_HOST,
        &bus_config,
        SDSPI_DEFAULT_DMA);
    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to initialize SD SPI bus: %s",
            esp_err_to_name(result));
        return result;
    }

    s_spi_bus_initialized = true;
    return ESP_OK;
}

/**
 * @brief Release the component-owned SPI bus after a failed mount/unmount.
 */
static esp_err_t sd_card_manager_release_spi_bus(void)
{
    if (!s_spi_bus_initialized)
    {
        return ESP_OK;
    }

    const esp_err_t result = spi_bus_free(SD_SPI_HOST);
    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to release SD SPI bus: %s",
            esp_err_to_name(result));
        return result;
    }

    s_spi_bus_initialized = false;
    return ESP_OK;
}

/**
 * @brief Mount the card through the existing SDSPI/FAT VFS owner seam.
 */
static esp_err_t sd_card_manager_mount_filesystem(void)
{
    if (!s_spi_bus_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_vfs_mounted)
    {
        return ESP_OK;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = SD_ALLOCATION_UNIT_SIZE,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_CLOCK_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_GPIO_CS;
    slot_config.host_id = SD_SPI_HOST;

    sdmmc_card_t *card = NULL;
    ESP_LOGI(TAG, "Mounting SD card filesystem at %s", SD_MOUNT_POINT);

    const esp_err_t result = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &card);
    if (result != ESP_OK)
    {
        /* ESP-IDF cleans the partially attached SDSPI card on mount failure. */
        s_card = NULL;
        return result;
    }

    s_card = card;
    s_vfs_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

/**
 * @brief Attempt one clean mount cycle from the recovery task.
 */
static esp_err_t sd_card_manager_mount_once(void)
{
    esp_err_t result = sd_card_manager_init_spi_bus();
    if (result != ESP_OK)
    {
        return result;
    }

    result = sd_card_manager_mount_filesystem();
    if (result == ESP_OK)
    {
        return ESP_OK;
    }

    const esp_err_t release_result = sd_card_manager_release_spi_bus();
    if (release_result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "SD mount cleanup also failed: %s",
            esp_err_to_name(release_result));
    }

    return result;
}

/**
 * @brief Unmount only after the recovery state has blocked and drained leases.
 *
 * esp_vfs_fat_sdcard_unmount() releases the card allocation even if its final
 * VFS unregister step returns an error, so this function invalidates local
 * card ownership before inspecting that result.
 */
static esp_err_t sd_card_manager_unmount_for_recovery(void)
{
    esp_err_t result = ESP_OK;

    if (s_vfs_mounted)
    {
        sdmmc_card_t *const card = s_card;
        s_card = NULL;
        s_vfs_mounted = false;

        if (card == NULL)
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else
        {
            result = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
        }

        if (result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "SD VFS unmount reported: %s",
                esp_err_to_name(result));
        }
    }

    const esp_err_t release_result = sd_card_manager_release_spi_bus();
    if ((result == ESP_OK) && (release_result != ESP_OK))
    {
        result = release_result;
    }

    return result;
}

static void sd_card_manager_notify_recovery_task(void)
{
    TaskHandle_t recovery_task = NULL;

    taskENTER_CRITICAL(&s_state_lock);
    recovery_task = s_recovery_task_handle;
    taskEXIT_CRITICAL(&s_state_lock);

    if (recovery_task != NULL)
    {
        xTaskNotifyGive(recovery_task);
    }
}

static void sd_card_manager_mark_mount_attempt(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_status.state = SD_CARD_MANAGER_STATE_MOUNTING;
    s_status.mount_attempt_count++;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void sd_card_manager_mark_mount_failure(
    esp_err_t error,
    bool in_initial_recovery_window)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_status.state = in_initial_recovery_window
                         ? SD_CARD_MANAGER_STATE_RETRY_WAIT
                         : SD_CARD_MANAGER_STATE_UNAVAILABLE;
    s_status.last_error = error;
    s_status.consecutive_mount_failures++;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void sd_card_manager_mark_initial_timeout(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_status.initial_recovery_timed_out = true;
    s_status.state = SD_CARD_MANAGER_STATE_UNAVAILABLE;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void sd_card_manager_mark_ready(bool initial_recovery_timed_out)
{
    taskENTER_CRITICAL(&s_state_lock);
    if (initial_recovery_timed_out)
    {
        s_status.initial_recovery_timed_out = true;
    }

    s_status.state = SD_CARD_MANAGER_STATE_READY;
    s_status.last_error = ESP_OK;
    s_status.consecutive_mount_failures = 0U;
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool sd_card_manager_initial_deadline_expired(TickType_t initial_start)
{
    return (xTaskGetTickCount() - initial_start) >=
           pdMS_TO_TICKS(SD_CARD_MANAGER_INITIAL_RECOVERY_TIMEOUT_MS);
}

static TickType_t sd_card_manager_initial_retry_wait_ticks(
    TickType_t initial_start)
{
    const TickType_t timeout_ticks =
        pdMS_TO_TICKS(SD_CARD_MANAGER_INITIAL_RECOVERY_TIMEOUT_MS);
    const TickType_t elapsed_ticks =
        xTaskGetTickCount() - initial_start;

    if (elapsed_ticks >= timeout_ticks)
    {
        return 0U;
    }

    const TickType_t remaining_ticks = timeout_ticks - elapsed_ticks;
    const TickType_t retry_ticks =
        pdMS_TO_TICKS(SD_CARD_MANAGER_INITIAL_RETRY_DELAY_MS);

    return (remaining_ticks < retry_ticks)
               ? remaining_ticks
               : retry_ticks;
}

/**
 * @brief Block new leases while a no-I/O idle health probe owns the card.
 */
static sdmmc_card_t *sd_card_manager_begin_idle_health_check(void)
{
    sdmmc_card_t *card = NULL;

    taskENTER_CRITICAL(&s_state_lock);
    if ((s_status.state == SD_CARD_MANAGER_STATE_READY) &&
        (s_status.active_leases == 0U) &&
        !s_health_check_in_progress &&
        s_vfs_mounted &&
        (s_card != NULL))
    {
        s_health_check_in_progress = true;
        card = s_card;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    return card;
}

/**
 * @brief Complete the idle health probe and request recovery on failure.
 *
 * @return true when the caller must continue immediately with recovery.
 */
static bool sd_card_manager_finish_idle_health_check(esp_err_t result)
{
    bool recovery_required = false;

    taskENTER_CRITICAL(&s_state_lock);
    s_health_check_in_progress = false;

    if ((result != ESP_OK) &&
        (s_status.state == SD_CARD_MANAGER_STATE_READY))
    {
        s_status.state = SD_CARD_MANAGER_STATE_RECOVERING;
        s_status.last_error = result;
        s_status.io_error_count++;
        recovery_required = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    return recovery_required;
}

/**
 * @brief Escalate a confirmed stdio/VFS media error without treating ENOENT as removal.
 */
static void sd_card_manager_report_errno_io_error(int error_number)
{
    if (sd_card_manager_is_vfs_media_error(error_number))
    {
        sd_card_manager_report_io_error(ESP_FAIL);
    }
}

static esp_err_t sd_card_manager_list_files_recursive_internal(
    const char *dir_path,
    uint8_t current_depth,
    uint8_t max_depth)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL)
    {
        const int open_errno = errno;
        ESP_LOGW(
            TAG,
            "Failed to open directory %s: errno=%d",
            dir_path,
            open_errno);
        sd_card_manager_report_errno_io_error(open_errno);
        return ESP_FAIL;
    }

    esp_err_t result = ESP_OK;
    int readdir_error = 0;

    while (true)
    {
        errno = 0;
        struct dirent *const entry = readdir(dir);
        if (entry == NULL)
        {
            readdir_error = errno;
            break;
        }

        if ((strcmp(entry->d_name, ".") == 0) ||
            (strcmp(entry->d_name, "..") == 0))
        {
            continue;
        }

        char full_path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
        const int written = snprintf(
            full_path,
            sizeof(full_path),
            "%s/%s",
            dir_path,
            entry->d_name);
        if ((written < 0) || (written >= (int)sizeof(full_path)))
        {
            ESP_LOGW(TAG, "Path too long, skipped: %s/%s", dir_path, entry->d_name);
            continue;
        }

        struct stat file_stat = {0};
        if (stat(full_path, &file_stat) != 0)
        {
            const int stat_errno = errno;
            ESP_LOGW(TAG, "Failed to stat %s: errno=%d", full_path, stat_errno);
            if (stat_errno == EIO)
            {
                sd_card_manager_report_errno_io_error(stat_errno);
                result = ESP_FAIL;
                break;
            }
            continue;
        }

        if (S_ISDIR(file_stat.st_mode))
        {
            ESP_LOGD(TAG, "[DIR ] depth=%u %s", (unsigned)current_depth, full_path);

            if (current_depth < max_depth)
            {
                const esp_err_t child_result =
                    sd_card_manager_list_files_recursive_internal(
                        full_path,
                        (uint8_t)(current_depth + 1U),
                        max_depth);
                if ((child_result != ESP_OK) && (result == ESP_OK))
                {
                    result = child_result;
                }
            }
        }
        else if (S_ISREG(file_stat.st_mode))
        {
            ESP_LOGD(
                TAG,
                "[FILE] depth=%u %s, size=%ld bytes",
                (unsigned)current_depth,
                full_path,
                (long)file_stat.st_size);
        }
        else
        {
            ESP_LOGD(TAG, "[OTHER] depth=%u %s", (unsigned)current_depth, full_path);
        }
    }

    if (readdir_error != 0)
    {
        ESP_LOGW(
            TAG,
            "Directory read failed for %s: errno=%d",
            dir_path,
            readdir_error);
        sd_card_manager_report_io_error(ESP_FAIL);
        result = ESP_FAIL;
    }

    if (closedir(dir) != 0)
    {
        ESP_LOGW(TAG, "Failed to close directory %s: errno=%d", dir_path, errno);
        sd_card_manager_report_io_error(ESP_FAIL);
        result = ESP_FAIL;
    }

    return result;
}

/**
 * @brief Own all card mount, health, unmount, and retry operations.
 *
 * No consumer may directly mount or unmount. A recovery request first blocks
 * new leases, then waits until LVGL/audio helpers have closed their handles.
 */
static void sd_card_manager_recovery_task(void *parameter)
{
    (void)parameter;

    TickType_t initial_start = 0U;
    bool initial_recovery_completed = false;
    bool initial_timeout_logged = false;

    ESP_LOGI(
        TAG,
        "Recovery task started: settle=%ums retry=%ums initial_timeout=%ums",
        (unsigned)SD_CARD_MANAGER_INITIAL_SETTLE_DELAY_MS,
        (unsigned)SD_CARD_MANAGER_INITIAL_RETRY_DELAY_MS,
        (unsigned)SD_CARD_MANAGER_INITIAL_RECOVERY_TIMEOUT_MS);

    vTaskDelay(pdMS_TO_TICKS(SD_CARD_MANAGER_INITIAL_SETTLE_DELAY_MS));
    initial_start = xTaskGetTickCount();

    while (true)
    {
        sd_card_manager_status_t status = {0};
        (void)sd_card_manager_get_status(&status);

        if (status.state == SD_CARD_MANAGER_STATE_READY)
        {
            const uint32_t notification_count = ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(SD_CARD_MANAGER_HEALTH_CHECK_INTERVAL_MS));
            if (notification_count != 0U)
            {
                continue;
            }

            sdmmc_card_t *const card = sd_card_manager_begin_idle_health_check();
            if (card == NULL)
            {
                continue;
            }

            const esp_err_t health_result = sdmmc_get_status(card);
            if (sd_card_manager_finish_idle_health_check(health_result))
            {
                ESP_LOGW(
                    TAG,
                    "Idle SD health check failed: %s; starting recovery",
                    esp_err_to_name(health_result));
            }

            continue;
        }

        if (status.state == SD_CARD_MANAGER_STATE_RECOVERING)
        {
            if (status.active_leases != 0U)
            {
                (void)ulTaskNotifyTake(
                    pdTRUE,
                    pdMS_TO_TICKS(SD_CARD_MANAGER_DRAIN_WAIT_MS));
                continue;
            }

            const esp_err_t cleanup_result = sd_card_manager_unmount_for_recovery();
            if (cleanup_result != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "SD recovery cleanup reported: %s",
                    esp_err_to_name(cleanup_result));
            }
        }

        const bool in_initial_recovery_window =
            !initial_recovery_completed &&
            !sd_card_manager_initial_deadline_expired(initial_start);

        if (!initial_recovery_completed &&
            !in_initial_recovery_window &&
            !initial_timeout_logged)
        {
            sd_card_manager_mark_initial_timeout();
            initial_timeout_logged = true;
            ESP_LOGW(
                TAG,
                "Initial SD recovery timed out; application continues and background retry remains active");
        }

        const uint32_t mount_attempt_number =
            status.mount_attempt_count + 1U;
        sd_card_manager_mark_mount_attempt();
        const esp_err_t mount_result = sd_card_manager_mount_once();
        if (mount_result == ESP_OK)
        {
            const bool completed_after_initial_deadline =
                !initial_recovery_completed &&
                sd_card_manager_initial_deadline_expired(initial_start);
            sd_card_manager_mark_ready(completed_after_initial_deadline);
            initial_recovery_completed = true;
            if (completed_after_initial_deadline)
            {
                ESP_LOGW(
                    TAG,
                    "SD filesystem became ready after the initial recovery deadline at %s",
                    SD_MOUNT_POINT);
            }
            else
            {
                ESP_LOGI(TAG, "SD filesystem is ready at %s", SD_MOUNT_POINT);
            }
            continue;
        }

        const bool retry_in_initial_window =
            !initial_recovery_completed &&
            !sd_card_manager_initial_deadline_expired(initial_start);
        sd_card_manager_mark_mount_failure(
            mount_result,
            retry_in_initial_window);

        if (retry_in_initial_window ||
            ((mount_attempt_number %
              SD_CARD_MANAGER_BACKGROUND_FAILURE_LOG_PERIOD) == 0U))
        {
            ESP_LOGW(
                TAG,
                "SD mount attempt %lu failed: %s%s",
                (unsigned long)mount_attempt_number,
                esp_err_to_name(mount_result),
                retry_in_initial_window
                    ? "; retrying in 2 seconds"
                    : "; retrying in background");
        }
        else
        {
            ESP_LOGD(
                TAG,
                "SD mount attempt %lu failed: %s",
                (unsigned long)mount_attempt_number,
                esp_err_to_name(mount_result));
        }

        if (retry_in_initial_window)
        {
            const TickType_t wait_ticks =
                sd_card_manager_initial_retry_wait_ticks(initial_start);
            if (wait_ticks != 0U)
            {
                (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
            }
        }
        else
        {
            (void)ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(SD_CARD_MANAGER_BACKGROUND_RETRY_DELAY_MS));
        }
    }
}

/* Functions ---------------------------------------------------------------- */
esp_err_t sd_card_manager_init(void)
{
    taskENTER_CRITICAL(&s_state_lock);

    if (s_initialized)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }

    if ((s_card != NULL) || s_vfs_mounted || s_spi_bus_initialized)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_status = (sd_card_manager_status_t){
        .state = SD_CARD_MANAGER_STATE_UNINITIALIZED,
        .last_error = ESP_OK,
    };
    s_health_check_in_progress = false;
    s_recovery_task_handle = NULL;
    s_initialized = true;

    taskEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG, "SD recovery service initialized");
    return ESP_OK;
}

esp_err_t sd_card_manager_start(void)
{
    taskENTER_CRITICAL(&s_state_lock);

    if (!s_initialized)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_started)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_OK;
    }

    s_started = true;
    s_status.state = SD_CARD_MANAGER_STATE_INITIALIZING;
    s_status.last_error = ESP_OK;

    taskEXIT_CRITICAL(&s_state_lock);

    TaskHandle_t recovery_task = NULL;
    const BaseType_t task_result = xTaskCreate(
        sd_card_manager_recovery_task,
        "sd_recovery",
        SD_CARD_MANAGER_TASK_STACK_SIZE_BYTES,
        NULL,
        SD_CARD_MANAGER_TASK_PRIORITY,
        &recovery_task);
    if (task_result != pdPASS)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_started = false;
        s_status.state = SD_CARD_MANAGER_STATE_UNINITIALIZED;
        s_status.last_error = ESP_ERR_NO_MEM;
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_recovery_task_handle = recovery_task;
    taskEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
}

esp_err_t sd_card_manager_get_status(sd_card_manager_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    const bool initialized = s_initialized;
    *status = s_status;
    taskEXIT_CRITICAL(&s_state_lock);

    return initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool sd_card_manager_is_mounted(void)
{
    bool mounted = false;

    taskENTER_CRITICAL(&s_state_lock);
    mounted = (s_status.state == SD_CARD_MANAGER_STATE_READY) &&
              !s_health_check_in_progress;
    taskEXIT_CRITICAL(&s_state_lock);

    return mounted;
}

esp_err_t sd_card_manager_acquire(void)
{
    esp_err_t result = ESP_ERR_INVALID_STATE;

    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized &&
        s_started &&
        (s_status.state == SD_CARD_MANAGER_STATE_READY) &&
        !s_health_check_in_progress)
    {
        s_status.active_leases++;
        result = ESP_OK;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    return result;
}

void sd_card_manager_release(void)
{
    bool release_without_lease = false;
    bool notify_recovery = false;

    taskENTER_CRITICAL(&s_state_lock);
    if (s_status.active_leases == 0U)
    {
        release_without_lease = true;
    }
    else
    {
        s_status.active_leases--;
        notify_recovery =
            (s_status.state == SD_CARD_MANAGER_STATE_RECOVERING) &&
            (s_status.active_leases == 0U);
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (release_without_lease)
    {
        ESP_LOGW(TAG, "SD lease release without a matching acquire");
        return;
    }

    if (notify_recovery)
    {
        sd_card_manager_notify_recovery_task();
    }
}

void sd_card_manager_report_io_error(esp_err_t error)
{
    if (error == ESP_OK)
    {
        return;
    }

    bool recovery_requested = false;

    taskENTER_CRITICAL(&s_state_lock);
    if (s_initialized &&
        (s_status.state == SD_CARD_MANAGER_STATE_READY))
    {
        s_status.state = SD_CARD_MANAGER_STATE_RECOVERING;
        s_status.last_error = error;
        s_status.io_error_count++;
        recovery_requested = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (recovery_requested)
    {
        ESP_LOGW(
            TAG,
            "SD I/O failure reported: %s; draining file leases before recovery",
            esp_err_to_name(error));
        sd_card_manager_notify_recovery_task();
    }
}

bool sd_card_manager_is_vfs_media_error(int error_number)
{
    return (error_number == EIO) ||
           (error_number == ENODEV) ||
           (error_number == ENXIO) ||
           (error_number == ETIMEDOUT);
}

esp_err_t sd_card_manager_write_test_file(void)
{
    esp_err_t result = sd_card_manager_acquire();
    if (result != ESP_OK)
    {
        return result;
    }

    const char *const file_path = SD_MOUNT_POINT "/hello.txt";
    FILE *file = fopen(file_path, "w");
    if (file == NULL)
    {
        const int open_errno = errno;
        ESP_LOGW(TAG, "Failed to open %s for writing: errno=%d", file_path, open_errno);
        sd_card_manager_report_errno_io_error(open_errno);
        sd_card_manager_release();
        return ESP_FAIL;
    }

    if ((fprintf(file, "Hello from ESP32-S3 SD card!\n") < 0) ||
        (fprintf(file, "SD clock: %d kHz\n", SD_CLOCK_KHZ) < 0) ||
        (fprintf(file, "Write/read test from sd_card_manager.\n") < 0))
    {
        result = ESP_FAIL;
        sd_card_manager_report_io_error(result);
    }

    if (fclose(file) != 0)
    {
        ESP_LOGW(TAG, "Failed to close %s: errno=%d", file_path, errno);
        result = ESP_FAIL;
        sd_card_manager_report_io_error(result);
    }

    sd_card_manager_release();

    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "File written successfully: %s", file_path);
    }

    return result;
}

esp_err_t sd_card_manager_read_test_file(void)
{
    esp_err_t result = sd_card_manager_acquire();
    if (result != ESP_OK)
    {
        return result;
    }

    const char *const file_path = SD_MOUNT_POINT "/hello.txt";
    FILE *file = fopen(file_path, "r");
    if (file == NULL)
    {
        const int open_errno = errno;
        ESP_LOGW(TAG, "Failed to open %s for reading: errno=%d", file_path, open_errno);
        sd_card_manager_report_errno_io_error(open_errno);
        sd_card_manager_release();
        return ESP_FAIL;
    }

    char line[128] = {0};
    while (fgets(line, sizeof(line), file) != NULL)
    {
        ESP_LOGD(TAG, "Read line: %s", line);
    }

    if (ferror(file))
    {
        result = ESP_FAIL;
        ESP_LOGW(TAG, "Read error for %s", file_path);
        sd_card_manager_report_io_error(result);
    }

    if (fclose(file) != 0)
    {
        ESP_LOGW(TAG, "Failed to close %s: errno=%d", file_path, errno);
        result = ESP_FAIL;
        sd_card_manager_report_io_error(result);
    }

    sd_card_manager_release();

    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "File read successfully: %s", file_path);
    }

    return result;
}

esp_err_t sd_card_manager_list_files(const char *dir_path)
{
    esp_err_t result = sd_card_manager_acquire();
    if (result != ESP_OK)
    {
        return result;
    }

    const char *const scan_path =
        (dir_path != NULL) ? dir_path : SD_MOUNT_POINT;
    DIR *dir = opendir(scan_path);
    if (dir == NULL)
    {
        const int open_errno = errno;
        ESP_LOGW(TAG, "Failed to open directory %s: errno=%d", scan_path, open_errno);
        sd_card_manager_report_errno_io_error(open_errno);
        sd_card_manager_release();
        return ESP_FAIL;
    }

    uint32_t file_count = 0U;
    uint32_t dir_count = 0U;
    int readdir_error = 0;

    while (true)
    {
        errno = 0;
        struct dirent *const entry = readdir(dir);
        if (entry == NULL)
        {
            readdir_error = errno;
            break;
        }

        char full_path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
        const int written = snprintf(
            full_path,
            sizeof(full_path),
            "%s/%s",
            scan_path,
            entry->d_name);
        if ((written < 0) || (written >= (int)sizeof(full_path)))
        {
            ESP_LOGW(TAG, "Path too long, skipped: %s/%s", scan_path, entry->d_name);
            continue;
        }

        struct stat file_stat = {0};
        if (stat(full_path, &file_stat) != 0)
        {
            const int stat_errno = errno;
            ESP_LOGW(TAG, "Failed to stat %s: errno=%d", full_path, stat_errno);
            if (stat_errno == EIO)
            {
                sd_card_manager_report_errno_io_error(stat_errno);
                result = ESP_FAIL;
                break;
            }
            continue;
        }

        if (S_ISDIR(file_stat.st_mode))
        {
            dir_count++;
            ESP_LOGD(TAG, "[DIR ] %s", full_path);
        }
        else if (S_ISREG(file_stat.st_mode))
        {
            file_count++;
            ESP_LOGD(TAG, "[FILE] %s, size=%ld bytes", full_path, (long)file_stat.st_size);
        }
        else
        {
            ESP_LOGD(TAG, "[OTHER] %s", full_path);
        }
    }

    if (readdir_error != 0)
    {
        result = ESP_FAIL;
        ESP_LOGW(
            TAG,
            "Directory read failed for %s: errno=%d",
            scan_path,
            readdir_error);
        sd_card_manager_report_io_error(result);
    }

    if (closedir(dir) != 0)
    {
        result = ESP_FAIL;
        ESP_LOGW(TAG, "Failed to close directory %s: errno=%d", scan_path, errno);
        sd_card_manager_report_io_error(result);
    }

    sd_card_manager_release();

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "List done. Files=%lu Directories=%lu",
            (unsigned long)file_count,
            (unsigned long)dir_count);
    }

    return result;
}

esp_err_t sd_card_manager_list_files_recursive(
    const char *dir_path,
    uint8_t max_depth)
{
    esp_err_t result = sd_card_manager_acquire();
    if (result != ESP_OK)
    {
        return result;
    }

    const char *const scan_path =
        (dir_path != NULL) ? dir_path : SD_MOUNT_POINT;
    ESP_LOGI(
        TAG,
        "Start recursive file listing from %s, max_depth=%u",
        scan_path,
        (unsigned)max_depth);

    result = sd_card_manager_list_files_recursive_internal(
        scan_path,
        0U,
        max_depth);
    sd_card_manager_release();

    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "Recursive file listing done");
    }

    return result;
}
