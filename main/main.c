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

/* Application ------------------------------------------------------------- */
void app_main(void)
{
    // Project information
    ESP_LOGI(TAG, "PROJECT: %s", APP_PROJECT_NAME);
    ESP_LOGI(TAG, "VERSION: %s", APP_PROJECT_VER);
    ESP_LOGI(TAG, "BUILD DATE: %s", APP_PROJECT_VER_DATE);

    // Display driver initialization
    esp_err_t ret = display_driver_init(&display_handle);

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
        ui_manager_lvgl_create_demo_screen();

        xTaskCreate(
            lvgl_task_handler,
            "lvgl_task",
            4096,
            NULL,
            5,
            NULL
        );
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
        uint32_t delay_ms = ui_manager_lvgl_task_handler(); // Call the LVGL task handler
        if(delay_ms == UINT32_MAX) {
            delay_ms = 5; // If no timer is ready, set a default delay
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms)); // Delay for the specified milliseconds
    }
}
