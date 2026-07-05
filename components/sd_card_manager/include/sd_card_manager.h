#pragma once

#include <stdbool.h>
#include <esp_err.h>

/**
 * @brief Initialize and mount the SD card filesystem.
 *
 * This function prepares the SPI bus configured in board_config.h, mounts the
 * SD card as a FAT filesystem, and makes it available through the configured
 * mount point, for example "/sdcard".
 *
 * The function is safe to call more than once. If the card is already mounted,
 * it returns ESP_OK without mounting again.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t sd_card_manager_init(void);

/**
 * @brief Check whether the SD card filesystem is currently mounted.
 *
 * This only reports the manager's internal mount state. It does not probe the
 * card socket again or verify that the card is still physically inserted.
 *
 * @return true if mounted, false otherwise.
 */
bool sd_card_manager_is_mounted(void);

/**
 * @brief Create or overwrite a small test file on the SD card.
 *
 * This is a bring-up helper used to verify that the mounted FAT filesystem can
 * write data successfully. The file path is fixed inside the implementation.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t sd_card_manager_write_test_file(void);

/**
 * @brief Read the SD card test file and print its content to the log.
 *
 * This is a bring-up helper paired with sd_card_manager_write_test_file().
 * It expects the test file to already exist.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t sd_card_manager_read_test_file(void);
