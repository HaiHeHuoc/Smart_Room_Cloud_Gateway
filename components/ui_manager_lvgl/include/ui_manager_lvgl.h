#pragma once

#include "esp_err.h"
#include "display_driver.h"

#include "lvgl.h"


/**
 * @brief Initialize LVGL, display buffers, flush synchronization, and tick timer.
 *
 * @param[in] diplay_handle Initialized display driver handle. The manager
 *            borrows this pointer, so its storage must remain valid.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid display handle,
 *         ESP_ERR_NO_MEM on allocation failure, or an ESP-IDF timer/LCD error.
 */
esp_err_t ui_manager_lvgl_init(display_driver_handle_t* diplay_handle);

/**
 * @brief Run one mutex-protected LVGL timer/rendering iteration.
 *
 * This function calls lv_timer_handler() once and does not wait between calls.
 */
void ui_manager_lvgl_task_handler(void);

/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void ui_manager_lvgl_create_demo_screen(void);

/**
 * @brief FreeRTOS demo task that updates and moves a counter label forever.
 *
 * @param vPrama Unused FreeRTOS task argument.
 */
void ui_manager_lvgl_running_demo(void* vPrama);

/**
 * @brief Start running demo task
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t ui_manager_lvgl_start_running_demo_task(void);

/**
 * @brief FreeRTOS task entry point that services LVGL approximately every 33 ms.
 *
 * @param param Unused FreeRTOS task argument.
 */
void lvgl_task_handler(void* param);

/**
 * @brief Start the main LVGL timer and rendering task.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t ui_manager_lvgl_start_UI_task(void);

/**
 * @brief Acquire the LVGL mutex, waiting indefinitely if necessary.
 *
 * @note Call only after ui_manager_lvgl_init(). Keep the protected section
 *       short and always pair this call with ui_manager_lvgl_release_mutex().
 */
void ui_manager_lvgl_wait_for_mutex(void);

/**
 * @brief Release the LVGL mutex acquired by ui_manager_lvgl_wait_for_mutex().
 */
void ui_manager_lvgl_release_mutex(void);
