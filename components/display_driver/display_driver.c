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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
static uint16_t display_driver_swap_rgb565_bytes(uint16_t color);

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
                        SPI_DMA_CH_AUTO
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
    
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_GPIO_DC,
        .cs_gpio_num = LCD_GPIO_CS,
        .pclk_hz     = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10
    };

    ESP_LOGI(TAG, "Creating LCD IOs");

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
        LCD_SPI_HOST,
        &io_config,
        &handle->io_handle
    ), TAG, "Failed to create LCD IOs, %s", __func__);


    ESP_LOGI(TAG, "Create ST7735 panel");

const esp_lcd_panel_dev_config_t panel_config = {
.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
.data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
    .reset_gpio_num = LCD_GPIO_RST,
    .vendor_config = NULL,
};

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st7735(
            handle->io_handle,
            &panel_config,
            &handle->panel_handle
        )
        ,TAG
        , "Fail to create ST7735 panel");

    ESP_LOGI(TAG, "Reset ST7735 panel");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_reset(handle->panel_handle),
        TAG,
        "Failed to reset LCD"
    );

    ESP_LOGI(TAG, "init ST7735 panel");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(handle->panel_handle),
        TAG,
        "Failed to init LCD"
    );



    ESP_LOGI(TAG, "Turn display on");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(handle->panel_handle, true),
        TAG,
        "Fail to turn LCD on"
    );

    vTaskDelay(pdMS_TO_TICKS(100));

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

    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(handle->panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "panel_handle is NULL");


    ESP_LOGI(TAG, "Start LCD raw color test");

const struct {
    const char *name;
    uint16_t color;
} tests[] = {
{"RED",   0xF800},
{"GREEN", 0x07E0},
{"BLUE",  0x001F},
{"WHITE", 0xFFFF},
{"BLACK", 0x0000},
};

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        ESP_LOGI(TAG, "Fill %s", tests[i].name);

        ESP_RETURN_ON_ERROR(
            display_driver_fill_color(handle, tests[i].color),
            TAG,
            "Fill color failed"
        );

        vTaskDelay(pdMS_TO_TICKS(800));
    }

    ESP_LOGI(TAG, "LCD raw color test done");

    return ESP_OK;

    /*
    Add your code to perform the raw color test on the display
    */
   
   return ESP_OK;
}

esp_err_t display_driver_fill_color(const display_driver_handle_t *handle, uint16_t color)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");
    ESP_RETURN_ON_FALSE(handle->panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");

    const int draw_lines = LCD_LVGL_DRAW_BUF_LINES;
    const size_t pixel_count = LCD_H_RES * draw_lines;
    const size_t buffer_size = pixel_count * sizeof(uint16_t);

    uint16_t *color_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(color_buffer != NULL, ESP_ERR_NO_MEM, TAG, "No memory for color buffer");

    for (size_t i = 0; i < pixel_count; ++i) {
        color_buffer[i] = display_driver_swap_rgb565_bytes(color);
    }

    for (int y = 0; y < LCD_V_RES; y += draw_lines) {
    int y_end = y + draw_lines;
    if (y_end > LCD_V_RES) {
    y_end = LCD_V_RES;
    }
        ret = esp_lcd_panel_draw_bitmap(
            handle->panel_handle,
            0,
            y,
            LCD_H_RES,
            y_end,
            color_buffer
        );

        if (ret != ESP_OK) {
            break;
        }

    }

    free(color_buffer);

    return ret;
}

uint16_t display_driver_swap_rgb565_bytes(uint16_t color)
{
    return (uint16_t)((color << 8) | (color >> 8));
}