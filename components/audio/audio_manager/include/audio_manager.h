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

/** Maximum copied WAV path size, including the terminating null byte. */
#define AUDIO_MANAGER_WAV_PATH_MAX_BYTES  256U

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

    /** A manager-owned recorded or WAV source is playing through MAX98357A. */
    AUDIO_MANAGER_STATE_PLAYBACK,

    /** The latest manager audio operation failed and will recover to IDLE. */
    AUDIO_MANAGER_STATE_ERROR
} audio_manager_state_t;

/** @brief Runtime audio settings copied during initialization. */
typedef struct
{
    /** Recording duration used by the current stability pipeline, in seconds. */
    uint32_t record_duration_seconds;

    /**
     * Playback volume in range 0..100. Recorded audio then uses its existing
     * conditioning; WAV audio uses this as a linear PCM16 scale only.
     */
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

    /** Result of the most recently completed manager audio operation. */
    esp_err_t last_error;

    /** Number of stability cycles that have started. */
    uint32_t cycles_started;

    /** Number of stability cycles that completed successfully. */
    uint32_t cycles_completed;

    /** Number of stability cycles that returned an error. */
    uint32_t cycles_failed;

    /** Number of WAV commands the manager task began handling. */
    uint32_t wav_playback_started;

    /** Number of WAV commands that reached normal EOF successfully. */
    uint32_t wav_playback_completed;

    /** Number of WAV commands that failed before or during playback. */
    uint32_t wav_playback_failed;

    /** Number of WAV commands cancelled by playback stop or manager stop. */
    uint32_t wav_playback_cancelled;

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

/**
 * @brief Return compile-selected NewSolution defaults.
 *
 * Production uses a five-second retained recording. When the default-off
 * golden stability Kconfig mode is selected, this function returns its
 * configured stress duration; the manager selects that mode's task priority
 * internally.
 */
audio_manager_config_t audio_manager_default_config(void);

/**
 * @brief Initialize audio ownership without starting the manager task.
 *
 * The function copies configuration, allocates the whole-recording PCM24 and
 * DSP workspace buffers in PSRAM, creates the bounded command queue/lifecycle
 * synchronization, and places the MAX98357A data pin in its safe LOW state.
 * No task or I2S channel remains active when initialization returns. Task
 * context only; do not call from an ISR.
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
 * @brief Start the single manager-owned production audio task.
 *
 * On success the task reaches IDLE and waits for bounded commands. Normal
 * production start does not begin capture, DSP, or playback. All source and
 * I2S ownership remains private to the component: the manager task owns I2S,
 * while an active private reader owns its WAV file and SD lease. A default-off
 * Kconfig
 * regression mode may run the existing golden stability cycle instead. Its
 * optional continuous WAV-stress coordinator is a separate test task that
 * only polls status and submits commands; it never owns I2S, a WAV stream, or
 * SD access. Active WAV playback uses a private bounded SD-to-PSRAM prefetch
 * worker; the manager remains the only I2S owner and joins that worker before
 * releasing the source. The call waits at most two seconds for manager-task
 * readiness. Serialize start/stop/deinit lifecycle calls in application code;
 * task context only.
 */
esp_err_t audio_manager_start(void);

/**
 * @brief Queue one canonical WAV file for asynchronous playback.
 *
 * The manager must be started and IDLE. path must be a null-terminated
 * absolute path below the SD mount and fit in
 * AUDIO_MANAGER_WAV_PATH_MAX_BYTES. The path is copied into bounded
 * manager-owned command storage before return; the caller may immediately
 * reuse its buffer. This function never opens the file or touches I2S and does
 * not block for playback duration. A pending/active operation or shutdown
 * returns ESP_ERR_INVALID_STATE; a full command queue or status-mutex timeout
 * returns ESP_ERR_TIMEOUT. Task context only; safe for the caller path buffer
 * to expire immediately after a successful return.
 */
esp_err_t audio_manager_play_wav(const char *path);

/**
 * @brief Request cancellation of the pending or active WAV playback.
 *
 * The caller only sets a bounded cancellation request. The manager task owns
 * source/TX cleanup; its private reader owns the file and checks cancellation
 * between bounded raw reads. Cancellation is observed after the current
 * synchronous filesystem or I2S operation returns, so it is not immediate or
 * preemptive.
 * Returns ESP_ERR_INVALID_STATE when no WAV command is pending/active. A
 * controlled cancellation completes with IDLE and ESP_OK in status. Task
 * context only.
 */
esp_err_t audio_manager_stop_playback(void);

/**
 * @brief Stop the manager task with a finite cooperative shutdown wait.
 *
 * Active WAV playback is cancelled by the manager task, which stops I2S then
 * joins the private reader before its source is released. When the default-off continuous WAV
 * stress test is selected, this also wakes and joins its non-I2S coordinator
 * task.
 * On success the state becomes INITIALIZED and audio_manager_start() may be
 * called again. The function returns ESP_ERR_TIMEOUT if a task cannot finish
 * its current bounded blocking operation and cleanup before the shutdown
 * deadline; it never force-deletes an owning task. The shutdown wait is five
 * seconds. Do not call this function from the audio manager callback/task
 * itself; serialize it with other lifecycle calls.
 */
esp_err_t audio_manager_stop(void);

/** @brief Copy the current manager status under a bounded mutex wait. */
esp_err_t audio_manager_get_status(
    audio_manager_status_t *status);

/**
 * @brief Deinitialize a stopped manager and release all owned resources.
 *
 * Call audio_manager_stop() first when the manager task is running. This
 * releases the command queue, lifecycle synchronization, PSRAM recording
 * buffer, DSP workspace, and status mutex after defensive audio cleanup. Task
 * context only; application code must ensure no concurrent lifecycle/status
 * call is using the manager when deinit begins.
 */
esp_err_t audio_manager_deinit(void);

/** @brief Convert one audio manager state into a stable log/UI string. */
const char *audio_manager_state_to_string(
    audio_manager_state_t state);

#ifdef __cplusplus
}
#endif
