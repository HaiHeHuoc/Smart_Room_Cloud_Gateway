#pragma once

#include "esp_err.h"
#include "display_driver.h"



esp_err_t ui_manager_lvgl_init(display_driver_handle_t* diplay_handle);
void ui_manager_lvgl_task_handler(void);
