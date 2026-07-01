#include "ui_manager_lvgl.h"

#include "esp_check.h"
#include "esp_log.h"

#include "lvgl.h"
#include "esp_timer.h"

/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
const static char *TAG = "UI_LVGL";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static display_driver_handle_t* s_display_handle = NULL;
static esp_timer_handle_t s_lvgl_tick_timer = NULL;

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */
static void ui_manager_lvgl_tick_cb(void *arg);

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */
static void ui_manager_lvgl_tick_cb(void *arg)
{
    lv_tick_inc(1); // Increment the LVGL tick count by 1 millisecond
}

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
esp_err_t ui_manager_lvgl_init(display_driver_handle_t* display_handle)
{
    esp_err_t ret = ESP_OK;

    // Validate the display handle and its panel handle before proceeding
    ESP_RETURN_ON_FALSE(display_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid display handle pointer");
    ESP_RETURN_ON_FALSE(display_handle->panel_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid panel handle pointer");

    ESP_LOGI(TAG, "Initialize LVGL UI manager");
    s_display_handle = display_handle; // Store the display handle for later use

    // Init lvgl core
    lv_init();

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
void ui_manager_lvgl_task_handler(void)
{
    // Call the LVGL timer handler to process LVGL tasks
    lv_timer_handler();
}
