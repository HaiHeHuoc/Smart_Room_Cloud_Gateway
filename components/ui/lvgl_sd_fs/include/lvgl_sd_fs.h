#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Register SD card filesystem driver to LVGL.
 *
 * After this function is called successfully, LVGL can access files
 * on the SD card using a drive-letter path:
 *
 * Example:
 *      "S:/Hinh.jpg"
 *      "S:/images/logo.png"
 *      "S:/gif/demo.gif"
 *
 * Internally, this driver maps:
 *
 *      "S:/Hinh.jpg"
 *
 * to:
 *
 *      "/sdcard/Hinh.jpg"
 *
 * Requirement before calling this function:
 *      - LVGL must already be initialized by lv_init().
 *
 * It is valid, and preferred during boot, to register before the SD VFS is
 * ready. The registered driver's ready callback stays false until
 * sd_card_manager reports a READY VFS, then becomes usable again after a safe
 * recovery mount. Do not call this from the SD recovery task.
 *
 * @return
 *      - ESP_OK on success
 */
esp_err_t lvgl_sd_fs_register(void);

/**
 * @brief Check whether LVGL SD filesystem driver has been registered.
 *
 * This only checks driver registration state.
 * It does not guarantee that the SD card is currently mounted.
 *
 * @return true if registered, false otherwise
 */
bool lvgl_sd_fs_is_registered(void);

/**
 * @brief Check whether LVGL SD filesystem is ready to use.
 *
 * Ready means:
 *      1. LVGL SD FS driver is registered
 *      2. SD card manager currently accepts new VFS leases
 *
 * @return true if ready, false otherwise
 */
bool lvgl_sd_fs_is_ready(void);
