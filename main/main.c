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

/* SD Management components ------------------------------------------------- */
#include "sd_card_manager.h"

/* LVGL SD Management ------------------------------------------------------- */
#include "lvgl_sd_fs.h"

/* LVGL Image Handler------------------------------------------------------- */
#include "lvgl_image_handler.h"

/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */
#define DISPLAY_IMAGE_TASK_STACK_SIZE_BYTES (12U * 1024U)
#define DISPLAY_IMAGE_TASK_PRIORITY         5U
#define TASK_STACK_WARNING_BYTES            (2U * 1024U)

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
static const char *TAG = "MAIN_APP";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static display_driver_handle_t display_handle;

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */
static void displayimage(void* arg);

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
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display driver: %s", esp_err_to_name(ret));
        return;
    }

    // Used to test the display by filling it with known colors. Uncomment to run the test.
    // ESP_ERROR_CHECK(display_driver_raw_color_test(&display_handle));

    // Initialize LVGL UI manager
    // Because LVGL core needs lvgl_init inside ui_manager_lvgl_init
    esp_err_t lvgl_ret = ui_manager_lvgl_init(&display_handle);


    // Initilize SD card manager
    esp_err_t sd_card_ret = sd_card_manager_init();
    if(sd_card_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Fail to initialize SD card driver");
        return;
    }
    else
    {
        // ESP_LOGI(TAG, "Start test SD card");
        // ESP_ERROR_CHECK(sd_card_manager_write_test_file());
        // ESP_ERROR_CHECK(sd_card_manager_read_test_file());
        // ESP_LOGI(TAG, "testing SD card is done");

        // Scan files inside specific folder
        sd_card_manager_list_files(NULL);

        ESP_ERROR_CHECK(lvgl_sd_fs_register());
    }

    if (lvgl_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL UI manager");
    }
    else
    {
        ESP_LOGI(TAG, "Start LVGL task handler");

        esp_err_t ui_task_ret = ui_manager_lvgl_start_UI_task();
        if (ui_task_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to start LVGL UI task: %s",
                     esp_err_to_name(ui_task_ret));
            return;
        }

        BaseType_t display_task_ret = xTaskCreate(
            displayimage,
            "Display image",
            DISPLAY_IMAGE_TASK_STACK_SIZE_BYTES,
            NULL,
            DISPLAY_IMAGE_TASK_PRIORITY,
            NULL
        );

        if (display_task_ret != pdPASS) {
            ESP_LOGE(TAG,
                     "Failed to create image task with %u-byte stack",
                     (unsigned int)DISPLAY_IMAGE_TASK_STACK_SIZE_BYTES);
            return;
        }

        ESP_LOGI(TAG,
                 "Image task started with %u-byte stack",
                 (unsigned int)DISPLAY_IMAGE_TASK_STACK_SIZE_BYTES);

        // ui_manager_lvgl_start_running_demo_task();
    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    while (1)
    {
        // ESP_LOGI(TAG, "Main loop running...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
}

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
static void displayimage(void* arg)
{
    (void)arg;

    while(1)
    {
        ui_manager_lvgl_wait_for_mutex();
        ESP_LOGI(TAG,"Displaying image");

        esp_err_t image_ret =
            lvgl_image_handler_show_jpg("S:/Hinh.jpg");

        ui_manager_lvgl_release_mutex();

        if (image_ret != ESP_OK) {
            ESP_LOGE(TAG,
                    "Failed to show JPG image: %s",
                    esp_err_to_name(image_ret));
        }

        const UBaseType_t minimum_free_stack =
            uxTaskGetStackHighWaterMark(NULL);

        if (minimum_free_stack < TASK_STACK_WARNING_BYTES) {
            ESP_LOGW(TAG,
                     "Image task minimum free stack is low: %u bytes",
                     (unsigned int)minimum_free_stack);
        }
        else {
            ESP_LOGI(TAG,
                     "Image task minimum free stack: %u bytes",
                     (unsigned int)minimum_free_stack);
        }

        vTaskDelay(pdMS_TO_TICKS(1'000'000));
        ui_manager_lvgl_wait_for_mutex();
        lvgl_image_handler_clear();
        ui_manager_lvgl_release_mutex();
    }

    // (void)arg;

    // TickType_t last_stack_log = 0;

    // while (1) {
    //     ui_manager_lvgl_wait_for_mutex();

    //     lv_timer_handler();

    //     ui_manager_lvgl_release_mutex();

    //     const TickType_t now = xTaskGetTickCount();

    //     if ((now - last_stack_log) >= pdMS_TO_TICKS(1000)) {
    //         ESP_LOGI(
    //             TAG,
    //             "lvgl_task minimum remaining stack: %u bytes",
    //             (unsigned int)uxTaskGetStackHighWaterMark(NULL)
    //         );

    //         last_stack_log = now;
    //     }

    //     vTaskDelay(pdMS_TO_TICKS(5));
    // }
}

// static void displayimage(void *arg)
// {
//     (void)arg;

//     ESP_LOGI(TAG, "Displaying JPG image");

//     ui_manager_lvgl_wait_for_mutex();

//     ESP_LOGI(
//         TAG,
//         "Display task minimum stack remaining=%u bytes",
//         (unsigned int)uxTaskGetStackHighWaterMark(NULL)
//     );
    
//     esp_err_t image_ret =
//         lvgl_image_handler_show_jpg("S:/Hinh.jpg");

//     ui_manager_lvgl_release_mutex();

//     if (image_ret != ESP_OK) {
//         ESP_LOGE(
//             TAG,
//             "Failed to show JPG image: %s",
//             esp_err_to_name(image_ret)
//         );
//     } else {
//         ESP_LOGI(TAG, "JPG image object created");
//     }

//     ESP_LOGI(
//         TAG,
//         "Display task minimum stack remaining=%u bytes",
//         (unsigned int)uxTaskGetStackHighWaterMark(NULL)
//     );

//     /*
//      * Deleting this task does not delete the LVGL image object.
//      * The object remains owned and rendered by LVGL.
//      */
//     vTaskDelete(NULL);
// }
