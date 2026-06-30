#include "display_driver.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_check.h"

#include <stdint.h>
#include <stdlib.h>

#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"


/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */
#define LCD_BITS_PER_PIXEL 16

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
const static char * TAG = "display_driver";

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */
static esp_err_t display_driver_init_backlight(void);
static esp_err_t display_driver_init_spi_bus(void);
static esp_err_t display_driver_fill_color(const display_driver_handle_t *handle, uint16_t color);

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */
esp_err_t display_driver_init_backlight(void)
{
    gpio_config_t m_BLConfig = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1UL << LCD_GPIO_BL),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_RETURN_ON_ERROR(gpio_config(&m_BLConfig), TAG, "Fail to configurate %s", __func__);

    return ESP_OK;
}


esp_err_t display_driver_init_spi_bus(void)
{
    // Seting SPI
    const spi_bus_config_t bus_config = {
        .miso_io_num = LCD_GPIO_MISO,
        .mosi_io_num = LCD_GPIO_MOSI,
        .sclk_io_num = LCD_GPIO_SCLK,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_MAX_TRANSFER_SIZE
    };

    ESP_LOGI(TAG, "spi bus initializes");

    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST,
                        &bus_config,
                        SPI_DMA_DISABLED
                        ), TAG, "Fail to initialize SPI bus");

    return ESP_OK;
}

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
esp_err_t display_driver_init(display_driver_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");

    
    /*
        Add your display driver initialization code here
            such as initializing the LCD panel and IO handles.
    */
    handle->io_handle = NULL;
    handle->panel_handle = NULL;

    ESP_RETURN_ON_ERROR(display_driver_init_backlight(), TAG, "Failed to init backlight");
    ESP_RETURN_ON_ERROR(display_driver_init_spi_bus(), TAG, "Failed to init SPI bus");
    
    ESP_RETURN_ON_ERROR(display_driver_set_backlight(true), TAG, "Failed to init backlight");


    ESP_LOGI(TAG, "Display Driver initialized successfully");
    return ESP_OK;
}

esp_err_t display_driver_set_backlight(bool enable)
{
    ESP_LOGI(TAG, "Setting backlight: %s", enable ? "ON" : "OFF");

    uint8_t m_u8BacklightState = enable ? LCD_BACKLIGHT_ON_LEVEL : LCD_BACKLIGHT_OFF_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_GPIO_BL, m_u8BacklightState),
                        TAG,
                        "Set backlight GPIO failed");

    return ESP_OK;
}

esp_err_t display_driver_raw_color_test(const display_driver_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");

    ESP_LOGI(TAG, "Performing raw color test on the display");

    /*
        Add your code to perform the raw color test on the display
    */

    return ESP_OK;
}
