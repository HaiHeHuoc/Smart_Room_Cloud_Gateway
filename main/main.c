#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

/* Common components ------------------------------------------------------- */
#include "app_common.h"
#include "board_config.h"

/* DISPLAY components ------------------------------------------------------- */
#include "display_driver.h"

/* LVGL Management components ----------------------------------------------- */
#include "ui_manager_lvgl.h"

/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
const char *TAG = "Main_App";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static display_driver_handle_t display_handle;

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */
void lvgl_task_handler(void* param);
void ui_manager_lvgl_running_demo(void* vPrama);

/* Application ------------------------------------------------------------- */
void app_main(void)
{
    // Project information
    ESP_LOGI(TAG, "PROJECT: %s", APP_PROJECT_NAME);
    ESP_LOGI(TAG, "VERSION: %s", APP_PROJECT_VER);
    ESP_LOGI(TAG, "BUILD DATE: %s", APP_PROJECT_VER_DATE);

    // Display driver initialization
    esp_err_t ret = display_driver_init(&display_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display driver: %s", esp_err_to_name(ret));
        return;
    }

    // Used to test the display by filling it with known colors. Uncomment to run the test.
    // ESP_ERROR_CHECK(display_driver_raw_color_test(&display_handle));

    // Initialize LVGL UI manager
    esp_err_t lvgl_ret = ui_manager_lvgl_init(&display_handle);

    if (lvgl_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL UI manager");
    }
    else
    {
        ESP_LOGI(TAG, "Start LVGL task handler");
        
        xTaskCreate(
            lvgl_task_handler,
            "lvgl_task",
            4096,
            NULL,
            5,
            NULL
        );
        xTaskCreate(
            ui_manager_lvgl_running_demo,
            "lvgl_task",
            4096,
            NULL,
            5,
            NULL
        );

        // ui_manager_lvgl_create_demo_screen();
    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    while (1)
    {
        ESP_LOGI(TAG, "Main loop running...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
}

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
void lvgl_task_handler(void* param)
{
    while(1)
    {
        ui_manager_lvgl_task_handler(); // Call the LVGL task handler
        vTaskDelay(pdMS_TO_TICKS(33)); // Delay for the specified milliseconds
    }
}

void ui_manager_lvgl_running_demo(void* vPrama) {
    ui_manager_lvgl_wait_for_mutex(); 
    lv_obj_t *screen = lv_screen_active(); 
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN); // Set background color to white 
    lv_obj_t *label = lv_label_create(screen); 
    lv_label_set_text(label, "LVGL OK"); 
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0); 
    lv_obj_center(label); 
    ui_manager_lvgl_release_mutex();
    uint8_t counter = 0; 
    while(1) { 
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        // Delay for 1 second 
        ui_manager_lvgl_wait_for_mutex();
        char text[20]; counter = (counter + 1) % 100; 
        // Increment counter and wrap around at 100 
        snprintf(text, sizeof(text), "Counter: %d", counter); 
        lv_label_set_text(label, text); ESP_LOGI(TAG, "Updated label text to: %s", text); 
        ui_manager_lvgl_release_mutex();
    } 
}
