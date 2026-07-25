#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Decode, aspect-fit, and show a JPEG file from the LVGL SD filesystem.
 *
 * @param[in] path LVGL filesystem path, for example "S:/images/photo.jpg".
 * @return ESP_OK on success, or an input, filesystem, decoder, allocation, or
 *         LVGL state error.
 * @note Call from the LVGL task or while holding the ui_manager_lvgl mutex.
 */
esp_err_t lvgl_image_handler_show_jpg(const char *path);

/**
 * @brief Stream-decode, aspect-fit, and show a PNG file from LVGL SD FS.
 *
 * @param[in] path LVGL filesystem path, for example "S:/images/photo.png".
 * @return ESP_OK on success, or an input, filesystem, decoder, allocation, or
 *         LVGL state error.
 * @note Call from the LVGL task or while holding the ui_manager_lvgl mutex.
 */
esp_err_t lvgl_image_handler_show_png(const char *path);

/**
 * @brief Start a looping animated GIF from the LVGL SD filesystem.
 *
 * @param[in] path LVGL filesystem path, for example "S:/images/demo.gif".
 * @return ESP_ERR_NOT_SUPPORTED when LV_USE_GIF is disabled; otherwise an
 *         input, state, allocation, or decoder error.
 * @note Call from the LVGL task or while holding the ui_manager_lvgl mutex.
 */
esp_err_t lvgl_image_handler_show_gif(const char *path);

/**
 * @brief Delete the active image and release its frame/decoder resources.
 *
 * @return ESP_OK. Calling this function when no image is active is allowed.
 * @note Call from the LVGL task or while holding the ui_manager_lvgl mutex.
 */
esp_err_t lvgl_image_handler_clear(void);

/**
 * @brief Check whether this component currently owns an LVGL image object.
 *
 * @return true when an image object is active, otherwise false.
 * @note The result is not synchronized. Call under the same LVGL ownership
 *       contract used by the show and clear APIs.
 */
bool lvgl_image_handler_has_active_object(void);

/**
 * @brief Start the built-in SD image cycling example task.
 *
 * The task expects `S:/Hinh.png`, `S:/Hinh.jpg`, and `S:/Hinh.gif`, and assumes
 * LVGL, SD card, and lvgl_sd_fs initialization are already complete. This is a
 * demonstration helper, not the application image-service task.
 *
 * @note The current void API does not report xTaskCreate() failure.
 */
void lvgl_image_handler_example_task(void);
