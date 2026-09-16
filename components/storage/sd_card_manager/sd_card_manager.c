/* Includes ----------------------------------------------------------------- */
#include "sd_card_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "app_log.h"
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
#define SD_CARD_MANAGER_DIRECTORY_LIST_SCAN_LIMIT             64U

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
static void (*s_availability_callback)(void);

static sd_card_manager_status_t s_status = {
    .state = SD_CARD_MANAGER_STATE_UNINITIALIZED,
    .last_error = ESP_OK,
};

#define SD_CARD_MANAGER_WEB_UPLOAD_TEMP_SUFFIX ".webupload-partial"

typedef enum { SD_WEB_TRANSFER_NONE = 0, SD_WEB_TRANSFER_DOWNLOAD, SD_WEB_TRANSFER_UPLOAD } sd_web_transfer_kind_t;
typedef struct {
    sd_web_transfer_kind_t kind;
    uint32_t id;
    FILE *file;
    char final_path[SD_CARD_MANAGER_PATH_MAX_LEN];
    char temporary_path[SD_CARD_MANAGER_PATH_MAX_LEN];
    uint64_t expected_size;
    uint64_t transferred_size;
} sd_web_transfer_t;
static sd_web_transfer_t s_web_transfer;
static uint32_t s_web_transfer_next_id = 1U;
/* One web mutation or transfer may hold the SD lease at a time.  This keeps
 * FATFS metadata changes deterministic while ordinary readers still use the
 * existing lease/recovery contract. */
static bool s_web_mutation_active = false;

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
static bool sd_card_manager_logical_path_is_valid(const char *logical_path);
static esp_err_t sd_card_manager_build_rooted_path(
    const char *logical_path,
    char *full_path,
    size_t full_path_size);
static esp_err_t sd_card_manager_error_from_errno(int error_number);
static bool sd_card_manager_component_has_reserved_suffix(
    const char *component,
    size_t component_length);
static esp_err_t sd_card_manager_web_transfer_claim(sd_web_transfer_kind_t kind, sd_web_transfer_t **transfer);
static void sd_card_manager_web_transfer_release(bool remove_temporary);
static esp_err_t sd_card_manager_web_mutation_claim(void);
static void sd_card_manager_web_mutation_release(void);

/* Static Functions --------------------------------------------------------- */
static void sd_card_manager_notify_availability(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    void (*callback)(void) = s_availability_callback;
    taskEXIT_CRITICAL(&s_state_lock);
    if (callback != NULL) callback();
}

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
        APP_LOGW(
            TAG, FAILED_TO_INITIALIZE_SD_SPI_A7CDBF50,
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
        APP_LOGW(
            TAG, FAILED_TO_RELEASE_SD_SPI_426FE55B,
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
    APP_LOGI(TAG, MOUNTING_SD_CARD_FILESYSTEM_E435C11A, "Mounting SD card filesystem at %s", SD_MOUNT_POINT);

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
        APP_LOGW(
            TAG, SD_MOUNT_CLEANUP_ALSO_FAILED_5D25A9EB,
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
            APP_LOGW(
                TAG, SD_VFS_UNMOUNT_REPORTED_S_5240B268,
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
    sd_card_manager_notify_availability();
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

    if (recovery_required) sd_card_manager_notify_availability();
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
        APP_LOGW(
            TAG, FAILED_TO_OPEN_DIRECTORY_S_458EB88B,
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
            APP_LOGW(TAG, PATH_TOO_LONG_SKIPPED_S_E2B96C85, "Path too long, skipped: %s/%s", dir_path, entry->d_name);
            continue;
        }

        struct stat file_stat = {0};
        if (stat(full_path, &file_stat) != 0)
        {
            const int stat_errno = errno;
            APP_LOGW(TAG, FAILED_TO_STAT_S_ERRNO_00230488, "Failed to stat %s: errno=%d", full_path, stat_errno);
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
            APP_LOGD(TAG, DIR_DEPTH_U_S_543DAB35, "[DIR ] depth=%u %s", (unsigned)current_depth, full_path);

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
            APP_LOGD(
                TAG, FILE_DEPTH_U_S_SIZE_92436265,
                "[FILE] depth=%u %s, size=%ld bytes",
                (unsigned)current_depth,
                full_path,
                (long)file_stat.st_size);
        }
        else
        {
            APP_LOGD(TAG, OTHER_DEPTH_U_S_7AE82EAE, "[OTHER] depth=%u %s", (unsigned)current_depth, full_path);
        }
    }

    if (readdir_error != 0)
    {
        APP_LOGW(
            TAG, DIRECTORY_READ_FAILED_FOR_S_53CE57F4,
            "Directory read failed for %s: errno=%d",
            dir_path,
            readdir_error);
        sd_card_manager_report_io_error(ESP_FAIL);
        result = ESP_FAIL;
    }

    if (closedir(dir) != 0)
    {
        APP_LOGW(TAG, FAILED_TO_CLOSE_DIRECTORY_S_3F53D9EB, "Failed to close directory %s: errno=%d", dir_path, errno);
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

    APP_LOGI(
        TAG, RECOVERY_TASK_STARTED_SETTLE_DD3741EB,
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
                APP_LOGW(
                    TAG, IDLE_SD_HEALTH_CHECK_FAILED_9B30FA9A,
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
                APP_LOGW(
                    TAG, SD_RECOVERY_CLEANUP_REPORTED_5FB737DA,
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
            APP_LOGW(
                TAG, INITIAL_SD_RECOVERY_TIMED_OU_006AE968,
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
                APP_LOGW(
                    TAG, SD_FILESYSTEM_BECAME_READY_A_9D39464B,
                    "SD filesystem became ready after the initial recovery deadline at %s",
                    SD_MOUNT_POINT);
            }
            else
            {
                APP_LOGI(TAG, SD_FILESYSTEM_IS_READY_AT_41152513, "SD filesystem is ready at %s", SD_MOUNT_POINT);
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
            APP_LOGW(
                TAG, SD_MOUNT_ATTEMPT_LU_FAILED_89BC80C7,
                "SD mount attempt %lu failed: %s%s",
                (unsigned long)mount_attempt_number,
                esp_err_to_name(mount_result),
                retry_in_initial_window
                    ? "; retrying in 2 seconds"
                    : "; retrying in background");
        }
        else
        {
            APP_LOGD(
                TAG, SD_MOUNT_ATTEMPT_LU_FAILED_E0D7AD69,
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
void sd_card_manager_register_availability_callback(void (*callback)(void))
{
    taskENTER_CRITICAL(&s_state_lock);
    s_availability_callback = callback;
    taskEXIT_CRITICAL(&s_state_lock);
}

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

    APP_LOGI(TAG, SD_RECOVERY_SERVICE_INITIALI_A922010C, "SD recovery service initialized");
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
        APP_LOGW(TAG, SD_LEASE_RELEASE_WITHOUT_A_B3701121, "SD lease release without a matching acquire");
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
        sd_card_manager_notify_availability();
        APP_LOGW(
            TAG, SD_I_O_FAILURE_REPORTED_D2CFB717,
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

/** Keep public storage browsing rooted below SD_MOUNT_POINT. */
static bool sd_card_manager_component_has_reserved_suffix(
    const char *component,
    size_t component_length)
{
    static const char suffix[] = SD_CARD_MANAGER_WEB_UPLOAD_TEMP_SUFFIX;
    const size_t suffix_length = sizeof(suffix) - 1U;
    if ((component == NULL) || (component_length < suffix_length))
    {
        return false;
    }

    const char *candidate = &component[component_length - suffix_length];
    for (size_t index = 0U; index < suffix_length; index++)
    {
        char character = candidate[index];
        if ((character >= 'A') && (character <= 'Z'))
        {
            character = (char)(character - 'A' + 'a');
        }
        if (character != suffix[index])
        {
            return false;
        }
    }
    return true;
}

static bool sd_card_manager_logical_path_is_valid(const char *logical_path)
{
    if ((logical_path == NULL) || (logical_path[0] != '/'))
    {
        return false;
    }

    const size_t length = strnlen(
        logical_path, SD_CARD_MANAGER_LOGICAL_PATH_MAX_LEN + 1U);
    if ((length == 0U) || (length > SD_CARD_MANAGER_LOGICAL_PATH_MAX_LEN))
    {
        return false;
    }

    if (strcmp(logical_path, "/") == 0)
    {
        return true;
    }
    if (logical_path[length - 1U] == '/')
    {
        return false;
    }

    const char *component = logical_path + 1;
    while (*component != '\0')
    {
        const char *const separator = strchr(component, '/');
        const size_t component_length =
            (separator == NULL) ? strlen(component) : (size_t)(separator - component);

        if ((component_length == 0U) ||
            (component_length > SD_CARD_MANAGER_DIRECTORY_ENTRY_NAME_MAX_LEN) ||
            ((component_length == 1U) && (component[0] == '.')) ||
            ((component_length == 2U) && (component[0] == '.') && (component[1] == '.')) ||
            sd_card_manager_component_has_reserved_suffix(
                component, component_length))
        {
            return false;
        }

        for (size_t index = 0U; index < component_length; index++)
        {
            const unsigned char character = (unsigned char)component[index];
            if ((character < 0x20U) || (character == '\\'))
            {
                return false;
            }
        }

        if (separator == NULL)
        {
            break;
        }
        component = separator + 1;
    }

    return true;
}

static esp_err_t sd_card_manager_build_rooted_path(
    const char *logical_path,
    char *full_path,
    size_t full_path_size)
{
    if ((full_path == NULL) || (full_path_size == 0U) ||
        !sd_card_manager_logical_path_is_valid(logical_path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(
        full_path, full_path_size, "%s%s", SD_MOUNT_POINT, logical_path);
    return ((written < 0) || ((size_t)written >= full_path_size))
               ? ESP_ERR_INVALID_SIZE
               : ESP_OK;
}

static esp_err_t sd_card_manager_error_from_errno(int error_number)
{
    if (error_number == ENOENT)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (error_number == ENOTDIR)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (error_number == ENOTEMPTY)
    {
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_FAIL;
}

static esp_err_t sd_card_manager_web_transfer_claim(sd_web_transfer_kind_t kind, sd_web_transfer_t **transfer)
{
    if (transfer == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if ((s_web_transfer.kind != SD_WEB_TRANSFER_NONE) || s_web_mutation_active)
    {
        taskEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_TIMEOUT;
    }
    s_web_transfer = (sd_web_transfer_t){
        .kind = kind,
        .id = s_web_transfer_next_id++,
    };
    if (s_web_transfer_next_id == 0U)
    {
        s_web_transfer_next_id = 1U;
    }
    *transfer = &s_web_transfer;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static void sd_card_manager_web_transfer_release(bool remove_temporary)
{
    if (s_web_transfer.file != NULL) {
        if (fclose(s_web_transfer.file) != 0)
        {
            sd_card_manager_report_io_error(ESP_FAIL);
        }
        s_web_transfer.file = NULL;
    }
    if (remove_temporary && (s_web_transfer.temporary_path[0] != '\0')) {
        if (unlink(s_web_transfer.temporary_path) != 0)
        {
            const int error = errno;
            if (error != ENOENT)
            {
                sd_card_manager_report_errno_io_error(error);
            }
        }
    }
    sd_card_manager_release();
    taskENTER_CRITICAL(&s_state_lock);
    s_web_transfer = (sd_web_transfer_t){0};
    taskEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t sd_card_manager_web_mutation_claim(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    const bool busy = (s_web_transfer.kind != SD_WEB_TRANSFER_NONE) ||
                      s_web_mutation_active;
    if (!busy)
    {
        s_web_mutation_active = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return busy ? ESP_ERR_TIMEOUT : ESP_OK;
}

static void sd_card_manager_web_mutation_release(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_web_mutation_active = false;
    taskEXIT_CRITICAL(&s_state_lock);
}

esp_err_t sd_card_manager_download_begin(const char *logical_path, sd_card_manager_transfer_info_t *info)
{
    if ((info == NULL) || !sd_card_manager_logical_path_is_valid(logical_path) ||
        (strcmp(logical_path, "/") == 0))
    {
        return ESP_ERR_INVALID_ARG;
    }
    *info = (sd_card_manager_transfer_info_t){0};
    sd_web_transfer_t *transfer = NULL;
    esp_err_t result = sd_card_manager_web_transfer_claim(SD_WEB_TRANSFER_DOWNLOAD, &transfer);
    if (result != ESP_OK) return result;
    if ((result = sd_card_manager_acquire()) != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_web_transfer = (sd_web_transfer_t){0};
        taskEXIT_CRITICAL(&s_state_lock);
        return result;
    }
    if ((result = sd_card_manager_build_rooted_path(logical_path, transfer->final_path, sizeof(transfer->final_path))) != ESP_OK) { sd_card_manager_web_transfer_release(false); return result; }
    struct stat file_stat = {0};
    if (stat(transfer->final_path, &file_stat) != 0) { const int error=errno; sd_card_manager_web_transfer_release(false); sd_card_manager_report_errno_io_error(error); return sd_card_manager_error_from_errno(error); }
    if (!S_ISREG(file_stat.st_mode)) { sd_card_manager_web_transfer_release(false); return ESP_ERR_NOT_SUPPORTED; }
    transfer->file = fopen(transfer->final_path, "rb");
    if (transfer->file == NULL) { const int error=errno; sd_card_manager_web_transfer_release(false); sd_card_manager_report_errno_io_error(error); return sd_card_manager_error_from_errno(error); }
    transfer->expected_size = (uint64_t)file_stat.st_size;
    *info = (sd_card_manager_transfer_info_t){ .transfer_id=transfer->id, .size_bytes=transfer->expected_size };
    return ESP_OK;
}

esp_err_t sd_card_manager_download_read(uint32_t transfer_id, void *buffer, size_t buffer_size, size_t *read_size)
{
    if ((buffer == NULL) || (read_size == NULL) || (buffer_size == 0U)) return ESP_ERR_INVALID_ARG;
    *read_size = 0U;
    if ((s_web_transfer.kind != SD_WEB_TRANSFER_DOWNLOAD) || (s_web_transfer.id != transfer_id) || (s_web_transfer.file == NULL)) return ESP_ERR_INVALID_STATE;
    const size_t read = fread(buffer, 1U, buffer_size, s_web_transfer.file);
    if ((read == 0U) && ferror(s_web_transfer.file)) { sd_card_manager_report_io_error(ESP_FAIL); return ESP_FAIL; }
    s_web_transfer.transferred_size += read; *read_size = read; return ESP_OK;
}

esp_err_t sd_card_manager_download_end(uint32_t transfer_id)
{
    if ((s_web_transfer.kind != SD_WEB_TRANSFER_DOWNLOAD) || (s_web_transfer.id != transfer_id)) return ESP_ERR_INVALID_STATE;
    const int close_result = fclose(s_web_transfer.file); s_web_transfer.file = NULL;
    if (close_result != 0) sd_card_manager_report_io_error(ESP_FAIL);
    sd_card_manager_release(); taskENTER_CRITICAL(&s_state_lock); s_web_transfer=(sd_web_transfer_t){0}; taskEXIT_CRITICAL(&s_state_lock);
    return close_result == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_card_manager_upload_begin(const char *logical_path, uint64_t content_length, sd_card_manager_transfer_info_t *info)
{
    if ((info == NULL) || !sd_card_manager_logical_path_is_valid(logical_path) ||
        (strcmp(logical_path, "/") == 0) || (content_length == 0U) ||
        (content_length > SD_CARD_MANAGER_TRANSFER_MAX_BYTES))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *info = (sd_card_manager_transfer_info_t){0};
    sd_web_transfer_t *transfer = NULL;
    esp_err_t result = sd_card_manager_web_transfer_claim(
        SD_WEB_TRANSFER_UPLOAD, &transfer);
    if (result != ESP_OK)
    {
        return result;
    }
    if ((result = sd_card_manager_acquire()) != ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_web_transfer = (sd_web_transfer_t){0};
        taskEXIT_CRITICAL(&s_state_lock);
        return result;
    }
    if ((result = sd_card_manager_build_rooted_path(
             logical_path, transfer->final_path, sizeof(transfer->final_path))) != ESP_OK)
    {
        sd_card_manager_web_transfer_release(false);
        return result;
    }

    struct stat existing = {0};
    if (stat(transfer->final_path, &existing) == 0)
    {
        sd_card_manager_web_transfer_release(false);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (errno != ENOENT)
    {
        const int error = errno;
        sd_card_manager_web_transfer_release(false);
        sd_card_manager_report_errno_io_error(error);
        return sd_card_manager_error_from_errno(error);
    }

    const int suffix = snprintf(
        transfer->temporary_path, sizeof(transfer->temporary_path),
        "%s%s", transfer->final_path,
        SD_CARD_MANAGER_WEB_UPLOAD_TEMP_SUFFIX);
    if ((suffix < 0) || (suffix >= (int)sizeof(transfer->temporary_path)))
    {
        sd_card_manager_web_transfer_release(false);
        return ESP_ERR_INVALID_SIZE;
    }
    (void)unlink(transfer->temporary_path);
    transfer->file = fopen(transfer->temporary_path, "wb");
    if (transfer->file == NULL)
    {
        const int error = errno;
        sd_card_manager_web_transfer_release(true);
        sd_card_manager_report_errno_io_error(error);
        return sd_card_manager_error_from_errno(error);
    }

    transfer->expected_size = content_length;
    *info = (sd_card_manager_transfer_info_t){
        .transfer_id = transfer->id,
        .size_bytes = content_length,
    };
    return ESP_OK;
}

esp_err_t sd_card_manager_upload_write(uint32_t transfer_id, const void *data, size_t data_size)
{
    if ((data == NULL) || (data_size == 0U)) return ESP_ERR_INVALID_ARG;
    if ((s_web_transfer.kind != SD_WEB_TRANSFER_UPLOAD) || (s_web_transfer.id != transfer_id) || (s_web_transfer.file == NULL)) return ESP_ERR_INVALID_STATE;
    if ((s_web_transfer.transferred_size + data_size) > s_web_transfer.expected_size) return ESP_ERR_INVALID_SIZE;
    if (fwrite(data, 1U, data_size, s_web_transfer.file) != data_size) { sd_card_manager_report_io_error(ESP_FAIL); return ESP_FAIL; }
    s_web_transfer.transferred_size += data_size; return ESP_OK;
}

esp_err_t sd_card_manager_upload_finish(uint32_t transfer_id)
{
    if ((s_web_transfer.kind != SD_WEB_TRANSFER_UPLOAD) || (s_web_transfer.id != transfer_id) || (s_web_transfer.transferred_size != s_web_transfer.expected_size)) return ESP_ERR_INVALID_STATE;
    const int close_result=fclose(s_web_transfer.file); s_web_transfer.file=NULL; if(close_result!=0){sd_card_manager_report_io_error(ESP_FAIL);sd_card_manager_web_transfer_release(true);return ESP_FAIL;}
    if(rename(s_web_transfer.temporary_path,s_web_transfer.final_path)!=0){const int error=errno;sd_card_manager_report_errno_io_error(error);sd_card_manager_web_transfer_release(true);return sd_card_manager_error_from_errno(error);}
    sd_card_manager_release(); taskENTER_CRITICAL(&s_state_lock);s_web_transfer=(sd_web_transfer_t){0};taskEXIT_CRITICAL(&s_state_lock); return ESP_OK;
}

void sd_card_manager_upload_abort(uint32_t transfer_id) { if ((s_web_transfer.kind == SD_WEB_TRANSFER_UPLOAD) && (s_web_transfer.id == transfer_id)) sd_card_manager_web_transfer_release(true); }

static esp_err_t sd_card_manager_mutate_one(const char *logical_path, int operation)
{
    if (!sd_card_manager_logical_path_is_valid(logical_path) ||
        (strcmp(logical_path, "/") == 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = sd_card_manager_web_mutation_claim();
    if (result != ESP_OK)
    {
        return result;
    }
    if ((result = sd_card_manager_acquire()) != ESP_OK)
    {
        sd_card_manager_web_mutation_release();
        return result;
    }

    char path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
    result = sd_card_manager_build_rooted_path(logical_path, path, sizeof(path));
    if (result == ESP_OK)
    {
        struct stat state = {0};
        if (stat(path, &state) != 0)
        {
            const int error = errno;
            result = sd_card_manager_error_from_errno(error);
            sd_card_manager_report_errno_io_error(error);
        }
        else if (((operation == 0) && !S_ISREG(state.st_mode)) ||
                 ((operation == 1) && !S_ISDIR(state.st_mode)))
        {
            result = ESP_ERR_NOT_SUPPORTED;
        }
        else if ((operation == 0 ? unlink(path) : rmdir(path)) != 0)
        {
            const int error = errno;
            result = sd_card_manager_error_from_errno(error);
            sd_card_manager_report_errno_io_error(error);
        }
    }
    sd_card_manager_release();
    sd_card_manager_web_mutation_release();
    return result;
}

esp_err_t sd_card_manager_delete_file(const char *logical_path) { return sd_card_manager_mutate_one(logical_path,0); }
esp_err_t sd_card_manager_remove_empty_directory(const char *logical_path) { return sd_card_manager_mutate_one(logical_path,1); }

esp_err_t sd_card_manager_make_directory(const char *logical_path)
{
    if (!sd_card_manager_logical_path_is_valid(logical_path) ||
        (strcmp(logical_path, "/") == 0))
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = sd_card_manager_web_mutation_claim();
    if (result != ESP_OK)
    {
        return result;
    }
    if ((result = sd_card_manager_acquire()) != ESP_OK)
    {
        sd_card_manager_web_mutation_release();
        return result;
    }
    char path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
    result = sd_card_manager_build_rooted_path(logical_path, path, sizeof(path));
    if ((result == ESP_OK) && (mkdir(path, 0775) != 0))
    {
        const int error = errno;
        result = (error == EEXIST) ? ESP_ERR_INVALID_RESPONSE :
                                     sd_card_manager_error_from_errno(error);
        sd_card_manager_report_errno_io_error(error);
    }
    sd_card_manager_release();
    sd_card_manager_web_mutation_release();
    return result;
}

esp_err_t sd_card_manager_rename_path(const char *source_logical_path, const char *destination_logical_path)
{
    if (!sd_card_manager_logical_path_is_valid(source_logical_path) ||
        !sd_card_manager_logical_path_is_valid(destination_logical_path) ||
        (strcmp(source_logical_path, "/") == 0) ||
        (strcmp(destination_logical_path, "/") == 0))
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = sd_card_manager_web_mutation_claim();
    if (result != ESP_OK)
    {
        return result;
    }
    if ((result = sd_card_manager_acquire()) != ESP_OK)
    {
        sd_card_manager_web_mutation_release();
        return result;
    }

    char source[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
    char destination[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
    result = sd_card_manager_build_rooted_path(
        source_logical_path, source, sizeof(source));
    if (result == ESP_OK)
    {
        result = sd_card_manager_build_rooted_path(
            destination_logical_path, destination, sizeof(destination));
    }
    struct stat state = {0};
    if ((result == ESP_OK) && (stat(source, &state) != 0))
    {
        const int error = errno;
        result = sd_card_manager_error_from_errno(error);
        sd_card_manager_report_errno_io_error(error);
    }
    if ((result == ESP_OK) && (stat(destination, &state) == 0))
    {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    else if ((result == ESP_OK) && (errno != ENOENT))
    {
        const int error = errno;
        result = sd_card_manager_error_from_errno(error);
        sd_card_manager_report_errno_io_error(error);
    }
    if ((result == ESP_OK) && (rename(source, destination) != 0))
    {
        const int error = errno;
        result = sd_card_manager_error_from_errno(error);
        sd_card_manager_report_errno_io_error(error);
    }
    sd_card_manager_release();
    sd_card_manager_web_mutation_release();
    return result;
}

esp_err_t sd_card_manager_get_filesystem_usage(
    sd_card_manager_filesystem_usage_t *usage)
{
    if (usage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *usage = (sd_card_manager_filesystem_usage_t){0};

    const esp_err_t lease_result = sd_card_manager_acquire();
    if (lease_result != ESP_OK)
    {
        return lease_result;
    }

    struct statvfs filesystem = {0};
    const int stat_result = statvfs(SD_MOUNT_POINT, &filesystem);
    const int stat_errno = errno;
    sd_card_manager_release();

    if (stat_result != 0)
    {
        sd_card_manager_report_errno_io_error(stat_errno);
        return sd_card_manager_error_from_errno(stat_errno);
    }

    const uint64_t block_size =
        (filesystem.f_frsize != 0U) ? filesystem.f_frsize : filesystem.f_bsize;
    usage->total_bytes = (uint64_t)filesystem.f_blocks * block_size;
    usage->free_bytes = (uint64_t)filesystem.f_bavail * block_size;
    usage->used_bytes = usage->total_bytes - usage->free_bytes;
    return ESP_OK;
}

esp_err_t sd_card_manager_list_directory(
    const char *logical_path,
    sd_card_manager_directory_listing_t *listing)
{
    if ((listing == NULL) || !sd_card_manager_logical_path_is_valid(logical_path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *listing = (sd_card_manager_directory_listing_t){0};
    snprintf(listing->path, sizeof(listing->path), "%s", logical_path);

    char full_path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
    const esp_err_t path_result = sd_card_manager_build_rooted_path(
        logical_path, full_path, sizeof(full_path));
    if (path_result != ESP_OK)
    {
        return path_result;
    }

    const esp_err_t lease_result = sd_card_manager_acquire();
    if (lease_result != ESP_OK)
    {
        return lease_result;
    }

    esp_err_t result = ESP_OK;
    DIR *directory = opendir(full_path);
    if (directory == NULL)
    {
        const int open_errno = errno;
        sd_card_manager_release();
        sd_card_manager_report_errno_io_error(open_errno);
        return sd_card_manager_error_from_errno(open_errno);
    }

    uint32_t scanned_entries = 0U;
    while (scanned_entries < SD_CARD_MANAGER_DIRECTORY_LIST_SCAN_LIMIT)
    {
        errno = 0;
        struct dirent *const entry = readdir(directory);
        if (entry == NULL)
        {
            const int read_errno = errno;
            if (read_errno != 0)
            {
                result = sd_card_manager_error_from_errno(read_errno);
                sd_card_manager_report_errno_io_error(read_errno);
            }
            break;
        }

        const size_t entry_name_length = strnlen(
            entry->d_name, SD_CARD_MANAGER_PATH_MAX_LEN);
        const bool is_web_partial =
            sd_card_manager_component_has_reserved_suffix(
                entry->d_name, entry_name_length);
        if ((strcmp(entry->d_name, ".") == 0) ||
            (strcmp(entry->d_name, "..") == 0) || is_web_partial)
        {
            continue;
        }

        scanned_entries++;
        const size_t name_length = strnlen(
            entry->d_name, SD_CARD_MANAGER_DIRECTORY_ENTRY_NAME_MAX_LEN + 2U);
        if ((name_length == 0U) ||
            (name_length > SD_CARD_MANAGER_DIRECTORY_ENTRY_NAME_MAX_LEN))
        {
            listing->unsupported_entry_count++;
            continue;
        }

        if (listing->entry_count >= SD_CARD_MANAGER_DIRECTORY_LIST_MAX_ENTRIES)
        {
            listing->truncated = true;
            break;
        }

        char entry_path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
        const int path_length = snprintf(
            entry_path, sizeof(entry_path), "%s/%s", full_path, entry->d_name);
        if ((path_length < 0) || (path_length >= (int)sizeof(entry_path)))
        {
            listing->unsupported_entry_count++;
            continue;
        }

        struct stat entry_stat = {0};
        if (stat(entry_path, &entry_stat) != 0)
        {
            const int stat_errno = errno;
            if (sd_card_manager_is_vfs_media_error(stat_errno))
            {
                result = sd_card_manager_error_from_errno(stat_errno);
                sd_card_manager_report_errno_io_error(stat_errno);
                break;
            }
            listing->unsupported_entry_count++;
            continue;
        }

        sd_card_manager_directory_entry_t *const output =
            &listing->entries[listing->entry_count++];
        memcpy(output->name, entry->d_name, name_length);
        output->name[name_length] = '\0';
        output->size_bytes = S_ISREG(entry_stat.st_mode)
                                 ? (uint64_t)entry_stat.st_size
                                 : 0U;
        output->type = S_ISDIR(entry_stat.st_mode)
                           ? SD_CARD_MANAGER_DIRECTORY_ENTRY_DIRECTORY
                           : (S_ISREG(entry_stat.st_mode)
                                  ? SD_CARD_MANAGER_DIRECTORY_ENTRY_FILE
                                  : SD_CARD_MANAGER_DIRECTORY_ENTRY_OTHER);
    }

    if (scanned_entries >= SD_CARD_MANAGER_DIRECTORY_LIST_SCAN_LIMIT)
    {
        listing->truncated = true;
    }

    const int close_result = closedir(directory);
    const int close_errno = errno;
    sd_card_manager_release();
    if (close_result != 0)
    {
        result = sd_card_manager_error_from_errno(close_errno);
        sd_card_manager_report_errno_io_error(close_errno);
    }

    return result;
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
        APP_LOGW(TAG, FAILED_TO_OPEN_S_FOR_ECCF8F02, "Failed to open %s for writing: errno=%d", file_path, open_errno);
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
        APP_LOGW(TAG, FAILED_TO_CLOSE_S_ERRNO_9C5A3777, "Failed to close %s: errno=%d", file_path, errno);
        result = ESP_FAIL;
        sd_card_manager_report_io_error(result);
    }

    sd_card_manager_release();

    if (result == ESP_OK)
    {
        APP_LOGI(TAG, FILE_WRITTEN_SUCCESSFULLY_S_A564BB95, "File written successfully: %s", file_path);
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
        APP_LOGW(TAG, FAILED_TO_OPEN_S_FOR_08870BD3, "Failed to open %s for reading: errno=%d", file_path, open_errno);
        sd_card_manager_report_errno_io_error(open_errno);
        sd_card_manager_release();
        return ESP_FAIL;
    }

    char line[128] = {0};
    while (fgets(line, sizeof(line), file) != NULL)
    {
        APP_LOGD(TAG, FILE_LINE_BYTES_U_EECC184F, "file_line_bytes=%u", (unsigned)strlen(line));
    }

    if (ferror(file))
    {
        result = ESP_FAIL;
        APP_LOGW(TAG, READ_ERROR_FOR_S_85FFCA2C, "Read error for %s", file_path);
        sd_card_manager_report_io_error(result);
    }

    if (fclose(file) != 0)
    {
        APP_LOGW(TAG, FAILED_TO_CLOSE_S_ERRNO_9C5A3777, "Failed to close %s: errno=%d", file_path, errno);
        result = ESP_FAIL;
        sd_card_manager_report_io_error(result);
    }

    sd_card_manager_release();

    if (result == ESP_OK)
    {
        APP_LOGI(TAG, FILE_READ_SUCCESSFULLY_S_E9BDF603, "File read successfully: %s", file_path);
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
        APP_LOGW(TAG, FAILED_TO_OPEN_DIRECTORY_S_ED57C648, "Failed to open directory %s: errno=%d", scan_path, open_errno);
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
            APP_LOGW(TAG, PATH_TOO_LONG_SKIPPED_S_E2B96C85, "Path too long, skipped: %s/%s", scan_path, entry->d_name);
            continue;
        }

        struct stat file_stat = {0};
        if (stat(full_path, &file_stat) != 0)
        {
            const int stat_errno = errno;
            APP_LOGW(TAG, FAILED_TO_STAT_S_ERRNO_00230488, "Failed to stat %s: errno=%d", full_path, stat_errno);
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
            APP_LOGD(TAG, DIR_S_5AA8CD77, "[DIR ] %s", full_path);
        }
        else if (S_ISREG(file_stat.st_mode))
        {
            file_count++;
            APP_LOGD(TAG, FILE_S_SIZE_LD_BYTES_7F15E610, "[FILE] %s, size=%ld bytes", full_path, (long)file_stat.st_size);
        }
        else
        {
            APP_LOGD(TAG, OTHER_S_5498AAC5, "[OTHER] %s", full_path);
        }
    }

    if (readdir_error != 0)
    {
        result = ESP_FAIL;
        APP_LOGW(
            TAG, DIRECTORY_READ_FAILED_FOR_S_53CE57F4,
            "Directory read failed for %s: errno=%d",
            scan_path,
            readdir_error);
        sd_card_manager_report_io_error(result);
    }

    if (closedir(dir) != 0)
    {
        result = ESP_FAIL;
        APP_LOGW(TAG, FAILED_TO_CLOSE_DIRECTORY_S_3F53D9EB, "Failed to close directory %s: errno=%d", scan_path, errno);
        sd_card_manager_report_io_error(result);
    }

    sd_card_manager_release();

    if (result == ESP_OK)
    {
        APP_LOGI(
            TAG, LIST_DONE_FILES_LU_DIRECTORI_95D3091D,
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
    APP_LOGI(
        TAG, START_RECURSIVE_FILE_LISTING_229792D5,
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
        APP_LOGI(TAG, RECURSIVE_FILE_LISTING_DONE_476EAA34, "Recursive file listing done");
    }

    return result;
}
