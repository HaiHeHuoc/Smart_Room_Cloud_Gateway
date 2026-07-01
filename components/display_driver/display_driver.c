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
/* ST7735 is configured for RGB565, which uses 16 bits for each pixel. */
#define LCD_BITS_PER_PIXEL 16

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
const static char * TAG = "DISPLAY_DRIVER";

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
    /* Configure the LCD backlight pin as a normal output GPIO. */
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
    /* The SPI bus carries LCD commands and pixel data to the ST7735 panel. */
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
    /* Caller owns the handle; this function fills in the LCD IO and panel handles. */
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");

    
    /*
        Add your display driver initialization code here
            such as initializing the LCD panel and IO handles.
    */
    /* Start from a known empty state before creating ESP-IDF LCD handles. */
    handle->io_handle = NULL;
    handle->panel_handle = NULL;

    /* Hardware-level setup must happen before creating the LCD panel object. */
    ESP_RETURN_ON_ERROR(display_driver_init_backlight(), TAG, "Failed to init backlight");
    ESP_RETURN_ON_ERROR(display_driver_init_spi_bus(), TAG, "Failed to init SPI bus");
    
    /* Panel IO maps ESP-IDF LCD transactions onto the SPI bus and LCD DC/CS pins. */
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

    /* Panel config describes how the ST7735 interprets pixel format and reset GPIO. */
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

    /* Reset and initialize the actual LCD controller before drawing to it. */
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



    /* Keep the panel enabled before turning on the backlight so the screen wakes cleanly. */
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

    /* Board config decides whether the backlight is active-high or active-low. */
    uint8_t m_u8BacklightState = enable ? LCD_BACKLIGHT_ON_LEVEL : LCD_BACKLIGHT_OFF_LEVEL;
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_GPIO_BL, m_u8BacklightState),
                        TAG,
                        "Set backlight GPIO failed");

    return ESP_OK;
}

esp_err_t display_driver_raw_color_test(const display_driver_handle_t *handle)
{
    /* This test fills the whole screen with known RGB565 colors one by one. */
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");
    
    ESP_LOGI(TAG, "Performing raw color test on the display");

    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(handle->panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "panel_handle is NULL");


    ESP_LOGI(TAG, "Start LCD raw color test");

    /* These values are standard RGB565 test colors: red, green, blue, white, black. */
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

        /* Draw one solid color, then wait so the result is visible on the LCD. */
        ESP_RETURN_ON_ERROR(
            display_driver_fill_color(handle, tests[i].color),
            TAG,
            "Fill color failed"
        );

        vTaskDelay(pdMS_TO_TICKS(800));
    }

    ESP_LOGI(TAG, "LCD raw color test done");

   return ESP_OK;
}

esp_err_t display_driver_fill_color(const display_driver_handle_t *handle, uint16_t color)
{
    esp_err_t ret = ESP_OK;

    /* A valid panel handle is required before any bitmap drawing operation. */
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");
    ESP_RETURN_ON_FALSE(handle->panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle pointer");

    /* Draw the screen in small horizontal chunks to keep DMA memory usage low. */
    const int draw_lines = LCD_LVGL_DRAW_BUF_LINES;
    const size_t pixel_count = LCD_H_RES * draw_lines;
    const size_t buffer_size = pixel_count * sizeof(uint16_t);

    /* Pixel buffer must be DMA-capable because the SPI driver reads from it directly. */
    uint16_t *color_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(color_buffer != NULL, ESP_ERR_NO_MEM, TAG, "No memory for color buffer");

    /* Pre-fill one chunk with the requested color so it can be reused for every band. */
    for (size_t i = 0; i < pixel_count; ++i) {
        color_buffer[i] = display_driver_swap_rgb565_bytes(color);
    }

    /* Sweep from top to bottom, drawing one chunk of rows at a time. */
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

esp_err_t display_driver_draw_bitmap(const display_driver_handle_t *handle,
                                     int x_start,
                                     int y_start,
                                     int x_end,
                                     int y_end,
                                     const void *color_data)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(handle!=NULL, ESP_ERR_INVALID_ARG,TAG , "invalid handle pointer");
    ESP_RETURN_ON_FALSE(handle->panel_handle!=NULL, ESP_ERR_INVALID_ARG,TAG , "invalid panel handle pointer");
    ESP_RETURN_ON_FALSE(color_data!=NULL, ESP_ERR_INVALID_ARG,TAG , "invalid color data pointer");

    ret = esp_lcd_panel_draw_bitmap(
        handle->panel_handle,
        x_start,
        y_start,
        x_end,
        y_end,
        color_data
    );

    return ret;

}

uint16_t display_driver_swap_rgb565_bytes(uint16_t color)
{
    /* ST7735/SPI transfers often expect RGB565 bytes in the opposite byte order. */
    return (uint16_t)((color << 8) | (color >> 8));
}
