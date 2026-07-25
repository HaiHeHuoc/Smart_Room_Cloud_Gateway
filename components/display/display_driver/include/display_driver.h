#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/**
 * @brief ESP-IDF handles owned by an initialized display driver instance.
 *
 * The caller allocates this structure, while display_driver_init() creates and
 * stores both handles. Keep the structure alive for every component that uses
 * the display. The current driver does not provide a matching deinit API.
 */
typedef struct {
    /** SPI panel IO used to submit commands and register transfer callbacks. */
    esp_lcd_panel_io_handle_t io_handle;

    /** Generic ESP LCD panel handle for the ST7735 controller. */
    esp_lcd_panel_handle_t panel_handle;
} display_driver_handle_t;

/**
 * @brief Initialize the LCD SPI bus, panel IO, ST7735 panel, and backlight.
 *
 * @param[out] handle Caller-owned storage that receives the created handles.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL handle, or an
 *         ESP-IDF driver/allocation error.
 *
 * @note The LCD SPI host must not already be initialized by another owner.
 */
esp_err_t display_driver_init(display_driver_handle_t *handle);

/**
 * @brief Display full-screen red, green, blue, white, and black test patterns.
 *
 * This blocking bring-up helper holds each color for approximately 800 ms.
 *
 * @param[in] handle Initialized display handle.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid handle, or a
 *         drawing/allocation error.
 */
esp_err_t display_driver_raw_color_test(const display_driver_handle_t *handle);

/**
 * @brief Submit an RGB565 bitmap rectangle to the ST7735 panel.
 *
 * End coordinates are exclusive, matching esp_lcd_panel_draw_bitmap(). The
 * pixel buffer must remain valid until the asynchronous panel IO transfer has
 * completed. ui_manager_lvgl handles that synchronization for LVGL flushes.
 *
 * @param[in] handle Initialized display handle.
 * @param x_start Inclusive left coordinate.
 * @param y_start Inclusive top coordinate.
 * @param x_end Exclusive right coordinate.
 * @param y_end Exclusive bottom coordinate.
 * @param[in] color_data RGB565 pixel data for the requested rectangle.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid pointers, or a
 *         panel IO error.
 */
esp_err_t display_driver_draw_bitmap(const display_driver_handle_t *handle,
                                     int x_start,
                                     int y_start,
                                     int x_end,
                                     int y_end,
                                     const void *color_data);
