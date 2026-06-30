#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"


typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} display_driver_handle_t;

esp_err_t display_driver_init(display_driver_handle_t *handle);

esp_err_t display_driver_set_backlight(bool enable);

esp_err_t display_driver_raw_color_test(const display_driver_handle_t *handle);
