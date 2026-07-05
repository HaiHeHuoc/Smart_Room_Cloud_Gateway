#pragma once

#include "esp_err.h"
#include "display_driver.h"

#include "lvgl.h"


/**
 * @brief LVGL initialization function. This function initializes the LVGL library, sets up the display, and configures the necessary buffers and callbacks.
 * 
 * @param display_handle 
 * @return esp_err_t 
 */
esp_err_t ui_manager_lvgl_init(display_driver_handle_t* diplay_handle);

/**
 * @brief Task handler for the LVGL task.
 * 
 * @return uint32_t 
 */
void ui_manager_lvgl_task_handler(void);

/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void ui_manager_lvgl_create_demo_screen(void);

/**
 * @brief Demo function for LVGL while running
 * 
 * @param vPrama 
 */
void ui_manager_lvgl_running_demo(void* vPrama);

/**
 * @brief Start running demo task
 * 
 * @param vPrama 
 */
void ui_manager_lvgl_start_running_demo_task(void);

/**
 * @brief LVGL task handler
 * 
 * @param param 
 */
void lvgl_task_handler(void* param);

/**
 * @brief Start running demo task
 * 
 */
void ui_manager_lvgl_start_UI_task();

/**
 * @brief Wait for the LVGL mutex to become available.
 * 
 */
void ui_manager_lvgl_wait_for_mutex(void);

/**
 * @brief Release the LVGL mutex.
 * 
 */
void ui_manager_lvgl_release_mutex(void);
