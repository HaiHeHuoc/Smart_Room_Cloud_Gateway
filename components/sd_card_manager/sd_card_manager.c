#include "sd_card_manager.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_check.h"

#include "esp_vfs_fat.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */
#define SD_MOUNT_MAX_RETRY        5
#define SD_MOUNT_RETRY_DELAY_MS   1000

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static const char* TAG = "sd_card_manager";

/*
 * SD card runtime state.
 *
 * s_card is owned by esp_vfs_fat_sdspi_mount() after a successful mount.
 * This component currently provides mount/read/write helpers only; an unmount
 * API can be added later when the application needs card removal support.
 */
static sdmmc_card_t *s_card = NULL;
static bool s_sd_mounted = false;
static bool s_spi_bus_initialized = false;

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

/**
 * @brief Initialize the SPI bus used by the SD card.
 *
 * The SD card is connected through SDSPI, so it uses the normal ESP-IDF SPI
 * master driver underneath. Pin mapping and host selection come from
 * board_config.h.
 *
 * This function only initializes the bus once. Calling it again returns ESP_OK
 * and leaves the existing bus configuration untouched.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
static esp_err_t sd_card_manager_init_spi_bus(void)
{
    if (s_spi_bus_initialized) {
        ESP_LOGW(TAG, "SD SPI bus is already initialized");
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

    /*
     * SDSPI_DEFAULT_DMA lets ESP-IDF choose a DMA channel automatically.
     * DMA is important because SD card transfers can be larger than simple
     * register-style SPI transactions.
     */
    ESP_LOGI(TAG, "spi bus initializes");

    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SD_SPI_HOST, &bus_config, SDSPI_DEFAULT_DMA),
        TAG,
        "Fail to init SPI bus");

    s_spi_bus_initialized = true;

    return ESP_OK;
}

/**
 * @brief Mount the SD card as a FAT filesystem.
 *
 * esp_vfs_fat_sdspi_mount() combines three jobs:
 * - Attach the card as an SDSPI device.
 * - Initialize the SD/MMC card protocol.
 * - Register a FAT filesystem under SD_MOUNT_POINT.
 *
 * File APIs such as fopen(), fgets(), and fprintf() can be used after this
 * succeeds.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
static esp_err_t sd_card_manager_mount_filesystem(void)
{
    ESP_RETURN_ON_FALSE(s_spi_bus_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "SD SPI bus is not initialized");

    if(s_sd_mounted == true)
    {
        ESP_LOGW(TAG, "SD card has been mounted!!!");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting SD card filesystem at %s", SD_MOUNT_POINT);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        /*
         * Keep formatting disabled for safety during bring-up. If mounting
         * fails, the card content will not be erased automatically.
         */
        .format_if_mount_failed = false,

        /*
         * Max number of files that can be opened at the same time through VFS.
         * Keep this small unless the application really needs many open files.
         */
        .max_files = SD_MAX_FILES,

        /*
         * FAT allocation unit size affects storage efficiency and performance.
         * 16 KB is a common starting point for SD cards.
         */
        .allocation_unit_size = SD_ALLOCATION_UNIT_SIZE,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    /*
     * The SDSPI host default config uses placeholder values. Override the host
     * slot and clock with the board-level SD card settings.
     */
    host.slot = SD_SPI_HOST;

    host.max_freq_khz = SD_CLOCK_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();

    /*
     * Only CS belongs to the SDSPI device config. MOSI/MISO/SCLK belong to the
     * SPI bus config created in sd_card_manager_init_spi_bus().
     */
    slot_config.gpio_cs = SD_GPIO_CS;
    slot_config.host_id = SD_SPI_HOST;

    ESP_RETURN_ON_ERROR(esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card),
        TAG,
        "Failed to mount"
    );

    s_sd_mounted = true;

    ESP_LOGI(TAG, "SD card mounted successfully");

    /*
     * Print card manufacturer, capacity, speed, and other useful bring-up
     * information to the serial monitor.
     */
    sdmmc_card_print_info(stdout, s_card);
    /*
     * Lower speed for first bring-up.
     * After SD is stable, we can increase this later.
     */

    return ESP_OK;
}

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */


/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
esp_err_t sd_card_manager_init(void)
{
    ESP_LOGI(TAG, "Start initlize sd_card_manager");

    esp_err_t ret = ESP_FAIL;
    /*
     * Avoid reinitializing the bus and remounting VFS if another caller already
     * initialized the SD card manager.
     */
    uint8_t m_ui8RetryCounter = SD_MOUNT_MAX_RETRY + 1;

    while(ret != ESP_OK && (--m_ui8RetryCounter > 0))
    {
        if(m_ui8RetryCounter != SD_MOUNT_MAX_RETRY)
        {
            ESP_LOGE(TAG, "Fail to init sd_card_manager_init, trying to do it again, trying times remains %d", m_ui8RetryCounter);
            vTaskDelay(pdMS_TO_TICKS(SD_MOUNT_RETRY_DELAY_MS));
        }

        if(s_sd_mounted)
        {
            ESP_LOGI(TAG, "SD card is mounted");
            return ESP_OK;
        }
        
        ESP_LOGW(TAG, "SD card is not mounted");
    
        /* Step 1: initialize the SPI bus before attaching the SD card device. */
        ret = sd_card_manager_init_spi_bus();
    
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SD SPI bus: %s", esp_err_to_name(ret));
            continue;
        }
    
        /* Step 2: mount the card and register the FAT filesystem in VFS. */
        ret = sd_card_manager_mount_filesystem();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount SD filesystem: %s", esp_err_to_name(ret));
            /*
             * If mount fails after the SPI bus was initialized, release the bus so
             * a later retry starts from a clean state.
             */
            spi_bus_free(SD_SPI_HOST);
            s_spi_bus_initialized = false;
            continue;
        }
    }

    return ret;
}

bool sd_card_manager_is_mounted(void)
{
    return s_sd_mounted;
}

esp_err_t sd_card_manager_write_test_file(void)
{
    ESP_RETURN_ON_FALSE(s_sd_mounted,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "SD card is not mounted");

    /*
     * This fixed test path is intentionally simple. Later application code
     * should add generic read/write APIs instead of hardcoding filenames here.
     */
    const char *file_path = SD_MOUNT_POINT "/hello.txt";

    ESP_LOGI(TAG, "Opening file for writing: %s", file_path);

    FILE *file = fopen(file_path, "w");
    ESP_RETURN_ON_FALSE(file != NULL,
                        ESP_FAIL,
                        TAG,
                        "Failed to open file for writing: %s",
                        file_path);

    fprintf(file, "Hello from ESP32-S3 SD card!\n");
    fprintf(file, "SD clock: %d kHz\n", SD_CLOCK_KHZ);
    fprintf(file, "Write/read test from sd_card_manager.\n");

    /*
     * fclose() flushes buffered stdio data to the FAT filesystem. Without it,
     * recently written data may not be committed before reading or power-off.
     */
    fclose(file);

    ESP_LOGI(TAG, "File written successfully: %s", file_path);

    return ESP_OK;
}

esp_err_t sd_card_manager_read_test_file(void)
{
    ESP_RETURN_ON_FALSE(s_sd_mounted,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "SD card is not mounted");

    const char *file_path = SD_MOUNT_POINT "/hello.txt";

    ESP_LOGI(TAG, "Opening file for reading: %s", file_path);

    FILE *file = fopen(file_path, "r");
    ESP_RETURN_ON_FALSE(file != NULL,
                        ESP_FAIL,
                        TAG,
                        "Failed to open file for reading: %s",
                        file_path);

    /*
     * Keep the line buffer small for embedded RAM usage. fgets() reads one line
     * at a time and prevents overflowing this buffer.
     */
    char line[128] = {0};

    while (fgets(line, sizeof(line), file) != NULL) {
        ESP_LOGI(TAG, "Read line: %s", line);
    }

    fclose(file);

    ESP_LOGI(TAG, "File read successfully: %s", file_path);

    return ESP_OK;
}
