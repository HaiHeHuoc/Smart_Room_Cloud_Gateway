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

/**
 * @brief List files and directories inside one SD card directory.
 *
 * This function opens the requested directory, reads each entry with readdir(),
 * checks its type/size with stat(), and prints the result to the ESP-IDF log.
 *
 * This is a non-recursive listing. It only prints entries directly inside
 * dir_path and does not enter subdirectories.
 *
 * @param dir_path Directory path to scan. Pass NULL to use SD_MOUNT_POINT.
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t sd_card_manager_list_files(const char *dir_path);

/**
 * @brief Recursively list files and directories inside an SD card directory.
 *
 * This function logs each discovered file, directory, and other filesystem
 * entry. It is intended for debugging and SD card bring-up, not for returning a
 * file list to application code.
 *
 * max_depth controls how far the scanner can enter subdirectories:
 * - 0 scans only the starting directory.
 * - 1 scans the starting directory and its direct child directories.
 * - Higher values allow deeper traversal.
 *
 * @param dir_path Directory path to scan. Pass NULL to use SD_MOUNT_POINT.
 * @param max_depth Maximum subdirectory depth to scan.
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t sd_card_manager_list_files_recursive(const char *dir_path, uint8_t max_depth);
