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
 * Requirements before calling this function:
 *      1. LVGL must already be initialized by lv_init().
 *      2. SD card must already be mounted by sd_card_manager_init().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if SD card is not mounted
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
 *      2. SD card is currently mounted
 *
 * @return true if ready, false otherwise
 */
bool lvgl_sd_fs_is_ready(void);
