/* Includes ----------------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_manager_lvgl.h"
#include "app_gui.h"
#include "esp_log.h"
#include "esp_err.h"

/* Macros ------------------------------------------------------------------- */
#define LVGL_DEMO_TASK_STACK_SIZE_BYTES (4U * 1024U)
#define LVGL_TASK_PRIORITY              5U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_GUI";

/* Application -------------------------------------------------------------- */
/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void app_gui_create_demo_screen(void)
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

/* Static Functions --------------------------------------------------------- */
/**
 * @brief Demo function for LVGL while running
 * 
 * @param vPrama 
 */
static void app_gui_running_demo(void* vPrama)
{
    (void)vPrama;

    const lv_align_t state[9] = {   
    LV_ALIGN_TOP_LEFT,
    LV_ALIGN_TOP_MID,
    LV_ALIGN_TOP_RIGHT,
    LV_ALIGN_LEFT_MID,
    LV_ALIGN_CENTER,
    LV_ALIGN_RIGHT_MID,
    LV_ALIGN_BOTTOM_LEFT,
    LV_ALIGN_BOTTOM_MID,
    LV_ALIGN_BOTTOM_RIGHT,
    };

    ui_manager_lvgl_wait_for_mutex(); 
    lv_obj_t *screen = lv_screen_active(); 
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // Set background color to white 
    lv_obj_t *label = lv_label_create(screen); 
    lv_label_set_text(label, "LVGL OK"); 
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0); 
    // lv_obj_center(label); 
    lv_obj_set_align(label, state[0]);
    ui_manager_lvgl_release_mutex();

    uint8_t counter = 0;

    while(1) { 
        vTaskDelay(pdMS_TO_TICKS(500)); 
        // Delay for 1 second 
        ui_manager_lvgl_wait_for_mutex();
        char text[20]; counter = (counter + 1) % 100; 
        // Increment counter and wrap around at 100 
        snprintf(text, sizeof(text), "Counter: %d", counter); 
        lv_label_set_text(label, text);
        // ESP_LOGI(TAG, "Updated label text to: %s", text); 
        lv_obj_set_align(label, state[counter%9]);
        ui_manager_lvgl_release_mutex();
    } 
}

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Start running demo task
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t app_gui_start_running_demo_task(void)
{
    BaseType_t task_ret = xTaskCreate(
        app_gui_running_demo,
        "lvgl_demo",
        LVGL_DEMO_TASK_STACK_SIZE_BYTES,
        NULL,
        LVGL_TASK_PRIORITY,
        NULL
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to create LVGL demo task with %u-byte stack",
                 (unsigned int)LVGL_DEMO_TASK_STACK_SIZE_BYTES);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "LVGL demo task started with %u-byte stack",
             (unsigned int)LVGL_DEMO_TASK_STACK_SIZE_BYTES);

    return ESP_OK;
}
