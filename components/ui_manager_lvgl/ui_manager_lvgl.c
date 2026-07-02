
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"

#include "ui_manager_lvgl.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

#define LCD_RORATE_PORTRAIT 0
#define LCD_RORATE_LANDSCAPE 1

#define LCD_ROTATE LCD_RORATE_LANDSCAPE

#ifndef LCD_ROTATE
    #define LCD_ROTATE LCD_RORATE_PORTRAIT
#endif

#ifndef LCD_ROTATE_ANGLE
    #if LCD_ROTATE == LCD_RORATE_LANDSCAPE
        #define LCD_ROTATE_ANGLE LV_DISPLAY_ROTATION_90
        // #define LCD_ROTATE_ANGLE LV_DISPLAY_ROTATION_270
    #else
        #define LCD_ROTATE_ANGLE LV_DISPLAY_ROTATION_0
    #endif
#endif


#define LVGL_RGB565_BYTES_PER_PIXEL  2
#define LVGL_DRAW_BUFFER_LINES       LCD_LVGL_DRAW_BUF_LINES
#if LCD_ROTATE == LCD_RORATE_LANDSCAPE
    #define LVGL_DRAW_BUFFER_WIDTH_MAX   LCD_V_RES
    #define LVGL_DRAW_BUFFER_SIZE        (LVGL_DRAW_BUFFER_WIDTH_MAX * LVGL_DRAW_BUFFER_LINES * LVGL_RGB565_BYTES_PER_PIXEL)
#else
    #define LVGL_DRAW_BUFFER_SIZE        (LCD_V_RES * LVGL_DRAW_BUFFER_LINES * LVGL_RGB565_BYTES_PER_PIXEL)
#endif

#ifndef LCD_SWAP_RGB565_BYTES
#define LCD_SWAP_RGB565_BYTES 1
#endif


/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
const static char *TAG = "UI_LVGL";
static SemaphoreHandle_t s_lvgl_mutex = NULL;

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static display_driver_handle_t* s_display_handle = NULL;
static esp_timer_handle_t s_lvgl_tick_timer = NULL;

static void *s_lvgl_draw_buffer = NULL;
#if LCD_ROTATE == LCD_RORATE_LANDSCAPE
    static void *s_lvgl_rotate_buffer  = NULL;
#endif

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */

/**
 * @brief LVGL tick callback function. This function is called periodically by the ESP timer to increment the LVGL tick count.
 * 
 * @param arg 
 */
static void ui_manager_lvgl_tick_cb(void *arg);

/**
 * @brief LVGL flush callback to render GUI content to the display. This function is called by LVGL when it needs to flush a portion of the display.
 * 
 * @param display 
 * @param area 
 * @param px_map 
 */
static void ui_manager_lvgl_flush_cb(lv_display_t *display,
                                     const lv_area_t *area,
                                     uint8_t *px_map);

/**
 * @brief Callback function to handle color transformation completion.
 * 
 * @param panel_io 
 * @param edata 
 * @param user_ctx 
 * @return true 
 * @return false 
 */
static bool ui_manager_lvgl_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                                esp_lcd_panel_io_event_data_t *edata,
                                                void *user_ctx);

/**
 * @brief Convert the byte order of RGB565 pixel data in a buffer.
 * 
 * @param buffer buffer containing RGB565 pixel data
 * @param pixel_count number of pixels in the buffer
 */
static void ui_manager_lvgl_swap_rgb565_bytes(uint16_t *buffer,
                                              uint32_t pixel_count);

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

/**
 * @brief LVGL tick callback function. This function is called periodically by the ESP timer to increment the LVGL tick count.
 * 
 * @param arg 
 */
static void ui_manager_lvgl_tick_cb(void *arg)
{
    lv_tick_inc(1); // Increment the LVGL tick count by 1 millisecond
}

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */

/**
 * @brief LVGL initialization function. This function initializes the LVGL library, sets up the display, and configures the necessary buffers and callbacks.
 * 
 * @param display_handle 
 * @return esp_err_t 
 */
esp_err_t ui_manager_lvgl_init(display_driver_handle_t* display_handle)
{
    esp_err_t ret = ESP_OK;

    // Validate the display handle and its panel handle before proceeding
    ESP_RETURN_ON_FALSE(display_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid display handle pointer");
    ESP_RETURN_ON_FALSE(display_handle->panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid panel handle pointer");

    ESP_LOGI(TAG, "Initialize LVGL UI manager");
    s_display_handle = display_handle; // Store the display handle for later use

    // Create a mutex for LVGL operations to ensure thread safety
    s_lvgl_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create LVGL mutex");

    // Init lvgl core
    lv_init();

    ESP_LOGW(TAG, "This is sizeof(lv_color_t): %d, sizeof(uint16_t): %d", sizeof(lv_color_t), sizeof(uint16_t));

    s_lvgl_draw_buffer = heap_caps_malloc(LVGL_DRAW_BUFFER_SIZE, MALLOC_CAP_DMA);

    ESP_RETURN_ON_FALSE(s_lvgl_draw_buffer != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "Failed to allocate LVGL draw buffer");

    #if LCD_ROTATE == LCD_RORATE_LANDSCAPE
        s_lvgl_rotate_buffer = heap_caps_malloc(LVGL_DRAW_BUFFER_SIZE,
                                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

        ESP_RETURN_ON_FALSE(s_lvgl_rotate_buffer != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "Failed to allocate LVGL rotate buffer");
    #endif

    gs_lvgl_display = lv_display_create(LCD_H_RES, LCD_V_RES);

    lv_display_set_buffers(gs_lvgl_display,
                        s_lvgl_draw_buffer,
                        NULL,
                        LVGL_DRAW_BUFFER_SIZE,
                        LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_RETURN_ON_FALSE(gs_lvgl_display != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "Failed to create LVGL display");

                        
    ESP_LOGI(TAG, "LVGL display created successfully with resolution: %dx%d", LCD_H_RES, LCD_V_RES);
    
    lv_display_set_color_format(gs_lvgl_display, LV_COLOR_FORMAT_RGB565);
    ESP_LOGI(TAG, "LVGL display color format set to RGB565");

    #if LCD_ROTATE == LCD_RORATE_LANDSCAPE
        lv_display_set_rotation(gs_lvgl_display, LCD_ROTATE_ANGLE);
    #endif
                        
    lv_display_set_flush_cb(gs_lvgl_display, ui_manager_lvgl_flush_cb);
    ESP_LOGI(TAG, "LVGL display flush callback set");

    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = ui_manager_lvgl_color_trans_done_cb,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(display_handle->io_handle,
                                                  &io_callbacks,
                                                  gs_lvgl_display),
        TAG,
        "Failed to register LCD IO callbacks"
    );

    // Create a periodic timer to call the LVGL tick increment function
    const esp_timer_create_args_t tick_timer_args = {
        .callback = ui_manager_lvgl_tick_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lvgl_tick_timer"
    };

    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &s_lvgl_tick_timer), 
                        TAG, 
                        "Failed to create LVGL tick timer");

    ESP_LOGI(TAG, "LVGL core and tick timer initialized");

    return ret;
}

/**
 * @brief Task handler for the LVGL task.
 * 
 */
uint32_t ui_manager_lvgl_task_handler(void)
{
    // waiting for the LVGL mutex to ensure thread safety
    ui_manager_lvgl_wait_for_mutex();
    ui_manager_lvgl_release_mutex();
    
    // Call the LVGL timer handler to process LVGL tasks
    return lv_timer_handler();
}

/**
 * @brief LVGL flush callback to render GUI content to the display. This function is called by LVGL when it needs to flush a portion of the display.
 * 
 * @param display 
 * @param area 
 * @param px_map 
 */
static void ui_manager_lvgl_flush_cb(lv_display_t *display,
                                     const lv_area_t *area,
                                     uint8_t *px_map)
{
    if(display == NULL)
    {
        ESP_LOGE(TAG, "Invalid display pointer");
        return;
    }

    const lv_area_t *draw_area = area;
    uint8_t *draw_buffer = px_map;

    #if LCD_ROTATE == LCD_RORATE_LANDSCAPE
        lv_color_format_t color_format = lv_display_get_color_format(display);
        lv_display_rotation_t rotation = lv_display_get_rotation(display);

        lv_area_t rotated_area;

        if (rotation != LV_DISPLAY_ROTATION_0) {
            rotated_area = *area;

            lv_display_rotate_area(display, &rotated_area);

            const int32_t src_width = lv_area_get_width(area);
            const int32_t src_height = lv_area_get_height(area);

            const uint32_t src_stride = lv_draw_buf_width_to_stride(src_width, color_format);
            const uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area),
                                                                        color_format);

            lv_draw_sw_rotate(px_map,
                                s_lvgl_rotate_buffer,
                                src_width,
                                src_height,
                                src_stride,
                                dest_stride,
                                rotation,
                                color_format);

            draw_area = &rotated_area;
            draw_buffer = (uint8_t *)s_lvgl_rotate_buffer;
        }
    #endif

    const int32_t width = draw_area->x2 - draw_area->x1 + 1;
    const int32_t height = draw_area->y2 - draw_area->y1 + 1;
    const uint32_t pixel_count = width * height;

    #if LCD_SWAP_RGB565_BYTES
        ui_manager_lvgl_swap_rgb565_bytes((uint16_t *)draw_buffer, pixel_count);
    #endif

    esp_err_t ret = display_driver_draw_bitmap(
        s_display_handle,
        draw_area->x1,
        draw_area->y1,
        draw_area->x2 + 1,
        draw_area->y2 + 1,
        draw_buffer
    );

    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to draw bitmap: %s", esp_err_to_name(ret));

        /*
         * If draw fails, notify LVGL anyway to avoid LVGL getting stuck.
         * On success, flush_ready will be called from color_trans_done callback.
         */
        lv_display_flush_ready(display);
    }
}

/**
 * @brief Callback function to handle color transformation completion.
 * 
 * @param panel_io 
 * @param edata 
 * @param user_ctx 
 * @return true 
 * @return false 
 */
static bool ui_manager_lvgl_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                                esp_lcd_panel_io_event_data_t *edata,
                                                void *user_ctx)
{
    (void)panel_io;
    (void)edata;

    lv_display_t *display = (lv_display_t *)user_ctx;

    if (display != NULL) {
        lv_display_flush_ready(display);
    }

    return false;
}

/**
 * @brief Convert the byte order of RGB565 pixel data in a buffer.
 * 
 * @param buffer buffer containing RGB565 pixel data
 * @param pixel_count number of pixels in the buffer
 */
static void ui_manager_lvgl_swap_rgb565_bytes(uint16_t *buffer,
                                              uint32_t pixel_count)
{
    for(uint32_t i = 0; i < pixel_count; ++i) {
        uint16_t color = buffer[i];
        buffer[i] = (color >> 8) | (color << 8); // Swap the bytes
    }
}

/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void ui_manager_lvgl_create_demo_screen(void)
{
    // waiting for the LVGL mutex to ensure thread safety
    ui_manager_lvgl_wait_for_mutex();

    lv_obj_t *screen = lv_screen_active();

    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // Set background color to white

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "LVGL OK");
    lv_obj_set_style_text_color(label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_obj_center(label);

    ui_manager_lvgl_release_mutex();
}

/**
 * @brief Wait for the LVGL mutex to become available.
 * 
 */
void ui_manager_lvgl_wait_for_mutex(void)
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
}

/**
 * @brief Release the LVGL mutex.
 * 
 */
void ui_manager_lvgl_release_mutex(void)
{
    xSemaphoreGive(s_lvgl_mutex);
}