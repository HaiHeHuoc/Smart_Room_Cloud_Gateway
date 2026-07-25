#pragma once

#include "esp_err.h"
#include "display_driver.h"

/**
 * @brief Initialize LVGL, display buffers, flush synchronization, and tick timer.
 *
 * @param[in] display_handle Initialized display driver handle. The manager
 *            borrows this pointer, so its storage must remain valid.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid display handle,
 *         ESP_ERR_INVALID_STATE if initialization was already attempted,
 *         ESP_ERR_NO_MEM on allocation failure, or an ESP-IDF timer/LCD error.
 */
esp_err_t ui_manager_lvgl_init(display_driver_handle_t *display_handle);

/**
 * @brief Acquire the LVGL mutex, waiting indefinitely if necessary.
 *
 * @note Call only after ui_manager_lvgl_init(). Keep the protected section
 *       short and always pair this call with ui_manager_lvgl_release_mutex().
 *       A call before initialization is logged and returns without blocking.
 */
void ui_manager_lvgl_wait_for_mutex(void);

/**
 * @brief Release the LVGL mutex acquired by ui_manager_lvgl_wait_for_mutex().
 *
 * A call before initialization is logged and ignored.
 */
void ui_manager_lvgl_release_mutex(void);
