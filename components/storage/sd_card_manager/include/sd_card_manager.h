#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_CARD_MANAGER_PATH_MAX_LEN 256U

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

/** Register a single composition-root availability hint callback (NULL removes).
 * Called after READY/recovery transitions, outside the state lock, in task
 * context. Must not block or perform I/O; read the current snapshot later.
 * Registration is independent of init. In-flight callbacks may finish after
 * removal, so callback code/context must remain valid for application lifetime.
 * The manager does not depend on any consumer component.
 */
void sd_card_manager_register_availability_callback(void (*callback)(void));

/**
 * @brief Reserve the mounted SD VFS for one open-file lifetime.
 *
 * A successful acquire increments the manager's active lease count. The caller
 * must pair it with sd_card_manager_release() after closing the file, including
 * every failure path after the acquire succeeds. Recovery waits for active
 * leases to drain before unmounting the VFS.
 *
 * @return ESP_OK when a lease is granted, ESP_ERR_INVALID_STATE before init(),
 *         or ESP_ERR_NOT_FOUND while the SD VFS is unavailable/recovering.
 */
esp_err_t sd_card_manager_acquire(void);

/**
 * @brief Release one previously acquired SD VFS lease.
 *
 * Calling this without a matching successful acquire is a programming error;
 * the manager ignores the underflow attempt and logs a diagnostic.
 */
void sd_card_manager_release(void);

/**
 * @brief Report a managed SD I/O failure and request recovery.
 *
 * Consumers call this after a file operation indicates a media/VFS failure.
 * The manager records the error, rejects new leases, waits for current leases
 * to drain, unmounts, and retries mounting in the background. Calls are
 * coalesced while recovery is already pending.
 *
 * @param error ESP-IDF error associated with the failed operation. ESP_OK is
 *              normalized to ESP_FAIL because this API represents a failure.
 */
void sd_card_manager_report_io_error(esp_err_t error);

/**
 * @brief Classify a stdio/VFS errno as likely SD-media unavailability.
 *
 * This helper keeps consumer error handling consistent without exposing the
 * recovery state machine. It is pure and safe from normal task context.
 *
 * @param error_number errno captured immediately after the failed VFS call.
 * @return true when the errno should trigger sd_card_manager_report_io_error().
 */
bool sd_card_manager_is_vfs_media_error(int error_number);
