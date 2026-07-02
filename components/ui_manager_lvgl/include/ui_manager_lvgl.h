#pragma once

#include "esp_err.h"
#include "display_driver.h"

#include "lvgl.h"

static lv_display_t *gs_lvgl_display = NULL;


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
uint32_t ui_manager_lvgl_task_handler(void);

/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void ui_manager_lvgl_create_demo_screen(void);

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