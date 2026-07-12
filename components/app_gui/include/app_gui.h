#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void app_gui_create_demo_screen(void);

/**
 * @brief Start running demo task
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t app_gui_start_running_demo_task(void);

