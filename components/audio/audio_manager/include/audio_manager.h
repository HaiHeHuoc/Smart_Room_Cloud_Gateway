#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Type Definitions --------------------------------------------------------- */

/** @brief Audio manager lifecycle and active pipeline state. */
typedef enum
{
    /** audio_manager_init() has not completed. */
    AUDIO_MANAGER_STATE_UNINITIALIZED = 0,

    /** Configuration, PSRAM buffers, and synchronization are ready. */
    AUDIO_MANAGER_STATE_INITIALIZED,

    /** Manager task is running and no capture/DSP/playback stage is active. */
    AUDIO_MANAGER_STATE_IDLE,

    /** INMP441 capture is active. */
    AUDIO_MANAGER_STATE_RECORDING,

    /** The recorded PCM24 buffer is being processed. */
    AUDIO_MANAGER_STATE_PROCESSING,

    /** The processed recording is being played through MAX98357A. */
    AUDIO_MANAGER_STATE_PLAYBACK,

    /** The latest stability cycle failed. The manager may recover to IDLE. */
    AUDIO_MANAGER_STATE_ERROR
} audio_manager_state_t;

/** @brief Runtime audio settings copied during initialization. */
typedef struct
{
    /** Recording duration used by the current stability pipeline, in seconds. */
    uint32_t record_duration_seconds;

    /** Playback volume percentage in range 0..100 before fixed conditioning. */
    uint32_t playback_volume_percent;
} audio_manager_config_t;

/** @brief Thread-safe snapshot suitable for diagnostics and GUI use. */
typedef struct
{
    /** Current lifecycle or active audio pipeline state. */
    audio_manager_state_t state;

    /** True while the manager has successfully enabled the I2S RX channel. */
    bool capture_i2s_active;

    /** True while the manager has successfully enabled the I2S TX channel. */
    bool playback_i2s_active;

    /** Result of the most recently completed stability cycle. */
    esp_err_t last_error;

    /** Number of stability cycles that have started. */
    uint32_t cycles_started;

    /** Number of stability cycles that completed successfully. */
    uint32_t cycles_completed;

    /** Number of stability cycles that returned an error. */
    uint32_t cycles_failed;

    /** Number of PCM24 samples captured by the most recent cycle. */
    size_t last_samples_recorded;

    /** Lifetime RX bytes requested from the I2S driver since initialization. */
    uint64_t rx_bytes_requested;

    /** Lifetime RX bytes returned by the I2S driver since initialization. */
    uint64_t rx_bytes_read;

    /** Lifetime TX bytes submitted to the I2S driver since initialization. */
    uint64_t tx_bytes_requested;

    /** Lifetime TX bytes accepted by the I2S driver since initialization. */
    uint64_t tx_bytes_written;

    /** RX DMA queue overflow callbacks observed since initialization. */
    uint32_t rx_overflow_count;

    /** RX read timeouts observed since initialization. */
    uint32_t rx_timeout_count;

    /** TX DMA queue overflow callbacks observed since initialization. */
    uint32_t tx_queue_overflow_count;

    /** TX write timeouts observed since initialization. */
    uint32_t tx_timeout_count;

    /** TX writes that returned fewer bytes than requested. */
    uint32_t tx_partial_write_count;

    /** Maximum measured blocking duration of an RX read call. */
    uint32_t max_rx_read_duration_us;

    /** Maximum measured blocking duration of a TX write call. */
    uint32_t max_tx_write_duration_us;

    /** Minimum remaining stack reported for the manager task, in bytes. */
    uint32_t task_stack_high_water_bytes;
} audio_manager_status_t;

/**
 * @brief Receive a copied audio status snapshot after a state/result change.
 *
 * The callback always executes in task context after the status mutex has been
 * released. During lifecycle calls it may execute in the caller task; during
 * runtime state changes it executes in the audio manager task. It is never
 * invoked from the I2S ISR callbacks. Keep it non-blocking. The status pointer
 * is temporary and must not be retained after the callback returns. This
 * contract supports the application-owned app_gui queue adapter without
 * allowing audio_manager to depend directly on LVGL or app_gui.
 */
typedef void (*audio_manager_status_callback_t)(
    const audio_manager_status_t *status,
    void *user_context);

/* Public API --------------------------------------------------------------- */

/** @brief Return the current known-good NewSolution defaults. */
audio_manager_config_t audio_manager_default_config(void);

/**
 * @brief Initialize audio ownership without starting the manager task.
 *
 * The function copies configuration, allocates the whole-recording PCM24 and
 * DSP workspace buffers in PSRAM, creates the status mutex, and places the
 * MAX98357A data pin in its safe LOW state. No I2S channel remains active when
 * initialization returns.
 */
esp_err_t audio_manager_init(
    const audio_manager_config_t *config);

/**
 * @brief Register or remove the single status callback.
 *
 * Passing NULL unregisters the callback. Registration is available after
 * audio_manager_init(). The callback is invoked only from task context and
 * never while the manager status mutex is held. Unregistration prevents new
 * callback selection, but does not wait for a callback snapshot that was
 * already selected before the call; its user context must remain valid until
 * the caller has externally synchronized with in-flight callbacks.
 */
esp_err_t audio_manager_register_status_callback(
    audio_manager_status_callback_t callback,
    void *user_context);

/**
 * @brief Start the single manager-owned audio task.
 *
 * The current Phase 11 NewSolution task continuously executes the proven
 * record -> DSP -> playback -> cleanup stability cycle. Task ownership and all
 * I2S operations remain private to audio_manager.
 */
esp_err_t audio_manager_start(void);

/** @brief Copy the current manager status under a bounded mutex wait. */
esp_err_t audio_manager_get_status(
    audio_manager_status_t *status);

/**
 * @brief Deinitialize the manager when its task has not been started.
 *
 * Runtime stop/deinit is intentionally not implemented for the infinite soak
 * task yet. Calling this after audio_manager_start() returns
 * ESP_ERR_INVALID_STATE.
 */
esp_err_t audio_manager_deinit(void);

/** @brief Convert one audio manager state into a stable log/UI string. */
const char *audio_manager_state_to_string(
    audio_manager_state_t state);

/**
 * @brief Compatibility alias for the pre-refactor NewSolution composition.
 *
 * New code and app_main call audio_manager_start(). This alias remains only
 * for legacy callers without changing audio behavior.
 */
esp_err_t audio_manager_test_start(void);

#ifdef __cplusplus
}
#endif
