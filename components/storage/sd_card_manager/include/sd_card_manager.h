#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Observable lifecycle state of the SD recovery service.
 *
 * The state describes VFS availability, not the physical card socket. This
 * board has no configured card-detect GPIO, so an idle card removal can only
 * be inferred from a later health probe or I/O failure.
 */
typedef enum
{
    /** sd_card_manager_init() has not completed. */
    SD_CARD_MANAGER_STATE_UNINITIALIZED = 0,

    /** The recovery task is applying the initial card-settle delay. */
    SD_CARD_MANAGER_STATE_INITIALIZING,

    /** The recovery task owns an in-progress SPI/FAT mount attempt. */
    SD_CARD_MANAGER_STATE_MOUNTING,

    /** The initial 90-second recovery window is still retrying. */
    SD_CARD_MANAGER_STATE_RETRY_WAIT,

    /** The VFS is mounted and new managed file leases may be acquired. */
    SD_CARD_MANAGER_STATE_READY,

    /** An I/O failure is draining managed file leases before unmounting. */
    SD_CARD_MANAGER_STATE_RECOVERING,

    /** Initial recovery timed out; background retry remains active. */
    SD_CARD_MANAGER_STATE_UNAVAILABLE,
} sd_card_manager_state_t;

/**
 * @brief Thread-safe diagnostic snapshot of the SD recovery service.
 *
 * Counters are lifetime values since sd_card_manager_init(). The snapshot is
 * copied by sd_card_manager_get_status(); callers do not own any component
 * resources and must not modify it.
 */
typedef struct
{
    sd_card_manager_state_t state;
    esp_err_t last_error;
    uint32_t mount_attempt_count;
    uint32_t consecutive_mount_failures;
    uint32_t io_error_count;
    uint32_t active_leases;
    bool initial_recovery_timed_out;
} sd_card_manager_status_t;

/**
 * @brief Prepare the SD recovery service without doing SPI or filesystem I/O.
 *
 * Call this once during application composition, before
 * sd_card_manager_start(). It is intentionally non-blocking so the GUI can
 * present its boot screen before the card begins its potentially slow initial
 * recovery. The recovery task is created by sd_card_manager_start().
 *
 * The function is idempotent after successful initialization.
 *
 * @return ESP_OK on success, or ESP_ERR_INVALID_STATE if the service is in an
 *         inconsistent lifecycle state.
 */
esp_err_t sd_card_manager_init(void);

/**
 * @brief Start the background SD mount and recovery task.
 *
 * The task waits briefly for a cold card to settle, retries mounting every two
 * seconds for an initial 90-second window, then keeps retrying in the
 * background without blocking the rest of the application. It also performs a
 * best-effort idle health probe and recovers after a managed I/O failure.
 *
 * This API is task-context only and is idempotent after a successful start.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before init(), or
 *         ESP_ERR_NO_MEM if the recovery task cannot be created.
 */
esp_err_t sd_card_manager_start(void);

/**
 * @brief Copy the current SD recovery diagnostic snapshot.
 *
 * This API is task-context only, does not perform card I/O, and is safe to
 * call while recovery is mounting, draining, or retrying.
 *
 * @param[out] status Receives one internally consistent snapshot.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for NULL, or
 *         ESP_ERR_INVALID_STATE before sd_card_manager_init().
 */
esp_err_t sd_card_manager_get_status(sd_card_manager_status_t *status);

/**
 * @brief Check whether the SD VFS is currently available for a new lease.
 *
 * This is a fast logical availability check. It does not reserve the VFS and
 * does not physically detect card presence. Call sd_card_manager_acquire()
 * immediately before opening a managed SD file to prevent an unmount race.
 *
 * @return true only while the manager is READY and accepts new leases.
 */
bool sd_card_manager_is_mounted(void);

/**
 * @brief Reserve the mounted SD VFS for one open-file lifetime.
 *
 * A successful caller must pair this with sd_card_manager_release() after the
 * corresponding FILE/DIR handle has been closed. Keep the lease from before
 * fopen()/opendir() until after fclose()/closedir(). The recovery task blocks
 * new leases and waits for all outstanding leases before unmounting the VFS.
 *
 * This API is non-ISR task-context code. It does not open a file or perform
 * media I/O.
 *
 * @return ESP_OK when the lease was reserved, ESP_ERR_INVALID_STATE when the
 *         VFS is not currently available.
 */
esp_err_t sd_card_manager_acquire(void);

/**
 * @brief Release one lease after its FILE/DIR handle is closed.
 *
 * This API is task-context only. It is safe to call while recovery is pending;
 * the final release wakes the recovery task so it can unmount safely.
 */
void sd_card_manager_release(void);

/**
 * @brief Notify the manager about a confirmed SD VFS I/O failure.
 *
 * Call this only after a real media/VFS error (for example ferror(), fseek(),
 * or fclose() failure), not for normal EOF or a missing file. New leases are
 * rejected immediately, while existing lease owners close their handles. The
 * manager then unmounts and retries in its own task; it never calls LVGL or
 * consumer callbacks.
 *
 * This API is non-blocking task-context code and is not ISR-safe.
 *
 * @param error Non-ESP_OK error that describes the observed failure.
 */
void sd_card_manager_report_io_error(esp_err_t error);

/**
 * @brief Classify a mounted FAT VFS errno that warrants SD recovery.
 *
 * ESP-IDF's FAT VFS maps disk/transport failure to `EIO`, an unavailable or
 * invalid drive/filesystem to `ENODEV`/`ENXIO`, and a media command timeout to
 * `ETIMEDOUT`. This helper recognizes those cases without doing I/O or
 * changing manager state. It lets consumers distinguish them from expected
 * file errors such as `ENOENT` before calling sd_card_manager_report_io_error().
 *
 * @param error_number errno captured immediately after a failed VFS call.
 * @return true when the error indicates a recoverable SD VFS/media failure.
 */
bool sd_card_manager_is_vfs_media_error(int error_number);

/**
 * @brief Create or overwrite a small test file on the SD card.
 *
 * This task-context bring-up helper acquires and releases its own VFS lease,
 * then writes a fixed implementation-owned test path. It requires the manager
 * to be READY and reports a confirmed write/close failure to recovery.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t sd_card_manager_write_test_file(void);

/**
 * @brief Read the SD card test file and print its content to the log.
 *
 * This task-context helper acquires and releases its own VFS lease. It is
 * paired with sd_card_manager_write_test_file() and expects the test file to
 * already exist.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
esp_err_t sd_card_manager_read_test_file(void);

/**
 * @brief List files and directories inside one SD card directory.
 *
 * This task-context helper acquires one VFS lease, opens the requested
 * directory, reads each entry with readdir(), checks type/size with stat(),
 * and prints the result to the ESP-IDF log.
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
 * This task-context helper holds one VFS lease while it logs each discovered
 * file, directory, and other filesystem entry. It is intended for debugging
 * and SD card bring-up, not for returning a file list to application code.
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
