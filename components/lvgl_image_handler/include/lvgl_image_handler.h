#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

/** Image formats explicitly supported by lvgl_image_handler_show(). */
typedef enum
{
    /** JPEG decoded through the configured LVGL image decoder. */
    LVGL_IMAGE_HANDLER_FORMAT_JPG = 0,
    /** PNG stream-decoded through libpng to limit peak source-buffer RAM. */
    LVGL_IMAGE_HANDLER_FORMAT_PNG,
    /** Animated GIF decoded frame-by-frame when LV_USE_GIF is enabled. */
    LVGL_IMAGE_HANDLER_FORMAT_GIF,
} lvgl_image_handler_format_t;

/**
 * @brief Decode and show an SD-backed image using an explicit format.
 *
 * The image is aspect-fitted to the active LVGL screen. A successful call
 * replaces the previously active image and transfers object/buffer ownership
 * to this component.
 *
 * @param[in] path LVGL filesystem path, for example "S:/images/photo.jpg".
 * @param format Decoder path to use; the filename extension is not inspected.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid input,
 *         ESP_ERR_INVALID_STATE if LVGL SD FS is not ready, ESP_ERR_NO_MEM on
 *         allocation failure, or a decoder-specific error.
 *
 * @note Call from the LVGL task or while holding the ui_manager_lvgl mutex.
 */
esp_err_t lvgl_image_handler_show(const char *path,
                                  lvgl_image_handler_format_t format);

/** @brief Show a JPEG file; see lvgl_image_handler_show(). */
esp_err_t lvgl_image_handler_show_jpg(const char *path);

/** @brief Stream-decode and show a PNG file; see lvgl_image_handler_show(). */
esp_err_t lvgl_image_handler_show_png(const char *path);

/**
 * @brief Start a looping animated GIF; see lvgl_image_handler_show().
 *
 * @return ESP_ERR_NOT_SUPPORTED when LV_USE_GIF is disabled; otherwise the
 *         same result set as lvgl_image_handler_show().
 */
esp_err_t lvgl_image_handler_show_gif(const char *path);

/**
 * @brief Delete the active image and release its frame/decoder resources.
 *
 * @return ESP_OK. Calling this function when no image is active is allowed.
 * @note Any pointer previously returned by lvgl_image_handler_get_image_obj()
 *       becomes invalid after this call.
 */
esp_err_t lvgl_image_handler_clear(void);

/** @return true when this component currently owns an LVGL image object. */
bool lvgl_image_handler_has_active_object(void);

/**
 * @brief Borrow the active image object for controlled LVGL operations.
 *
 * The returned pointer remains owned by this component. Do not delete it and
 * do not retain it across clear() or another successful show call.
 *
 * @return Active image object, or NULL when no image is available.
 */
lv_obj_t *lvgl_image_handler_get_image_obj(void);

/**
 * @brief Scale an image widget and align its scaled layout box to its parent.
 *
 * Scaling uses the image's current widget size as 100 percent, changes the
 * pivot to the top-left corner, and updates the widget size so corner
 * alignment reflects the visible scaled image.
 *
 * @param[in,out] image_obj Image object returned by this component.
 * @param percent Scale percentage in the inclusive range 1 through 100.
 * @param align LVGL parent-relative alignment.
 * @param offset_x Horizontal offset applied by lv_obj_align().
 * @param offset_y Vertical offset applied by lv_obj_align().
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid object or
 *         percentage, or ESP_ERR_INVALID_SIZE for an invalid widget size.
 */
esp_err_t lvgl_image_handler_apply_scale_and_align(
    lv_obj_t *image_obj,
    uint32_t percent,
    lv_align_t align,
    int32_t offset_x,
    int32_t offset_y);

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
