#include "sd_card_manager.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"

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

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static const char* TAG = "sd_card_manager";

/*
 * SD card runtime state.
 */
static sdmmc_card_t *s_card = NULL;
static bool s_sd_mounted = false;
static bool s_spi_bus_initialized = false;

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */
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

    ESP_LOGI(TAG, "spi bus initializes");

    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SD_SPI_HOST, &bus_config, SDSPI_DEFAULT_DMA),
        TAG,
        "Fail to init SPI bus");

    s_spi_bus_initialized = true;

    return ESP_OK;
}

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

    if(s_sd_mounted)
    {
        ESP_LOGI(TAG, "SD card is mounted");
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "SD card is not mounted");

    /*
     * TODO Step 1:
     * Implement sd_card_manager_init_spi_bus()
     */
    esp_err_t ret = sd_card_manager_init_spi_bus();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SD SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * TODO Step 2:
     * Implement sd_card_manager_mount_filesystem()
     */
    ret = sd_card_manager_mount_filesystem();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD filesystem: %s", esp_err_to_name(ret));

        spi_bus_free(SD_SPI_HOST);
        s_spi_bus_initialized = false;

        return ret;
    }

    return ESP_OK;
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

    ESP_LOGI(TAG, "TODO: write test file to SD card");

    /*
     * TODO Step 4:
     *
     * const char *file_path = SD_MOUNT_POINT "/hello.txt";
     *
     * FILE *file = fopen(file_path, "w");
     * ESP_RETURN_ON_FALSE(file != NULL,
     *                     ESP_FAIL,
     *                     TAG,
     *                     "Failed to open file for writing");
     *
     * fprintf(file, "Hello from ESP32-S3 SD card!\n");
     * fclose(file);
     *
     * ESP_LOGI(TAG, "File written: %s", file_path);
     */

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t sd_card_manager_read_test_file(void)
{
    ESP_RETURN_ON_FALSE(s_sd_mounted,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "SD card is not mounted");

    ESP_LOGI(TAG, "TODO: read test file from SD card");

    /*
     * TODO Step 5:
     *
     * const char *file_path = SD_MOUNT_POINT "/hello.txt";
     *
     * FILE *file = fopen(file_path, "r");
     * ESP_RETURN_ON_FALSE(file != NULL,
     *                     ESP_FAIL,
     *                     TAG,
     *                     "Failed to open file for reading");
     *
     * char line[128] = {0};
     * fgets(line, sizeof(line), file);
     * fclose(file);
     *
     * ESP_LOGI(TAG, "Read from file: %s", line);
     */

    return ESP_ERR_NOT_SUPPORTED;
}