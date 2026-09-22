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

/** Playback position is committed after each 256-frame manager TX block. */
#define AUDIO_MANAGER_PLAYBACK_POSITION_GRANULARITY_FRAMES  256U

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

    /**
     * A manager-owned recorded, WAV, or live PCM16 operation owns playback.
     * A controlled local-source PAUSED releases the manager operation slot and
     * reports lifecycle IDLE while its copied playback-control status remains
     * PAUSED. This permits PTT capture/TTS to use I2S without retaining FILE,
     * SD-reader, I2S, or DMA ownership for the local source.
     */
    AUDIO_MANAGER_STATE_PLAYBACK,

    /** The latest manager audio operation failed and will recover to IDLE. */
    AUDIO_MANAGER_STATE_ERROR
} audio_manager_state_t;

/** @brief Project-owned playback-control state, independent of I2S handles. */
typedef enum
{
    AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE = 0,
    AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING,
    AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING,
    AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING,
    AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED,
    AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING,
    AUDIO_MANAGER_PLAYBACK_CONTROL_STOPPING,
    AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR,
} audio_manager_playback_control_state_t;

/** @brief Bounded source identity exposed by playback-control snapshots. */
typedef enum
{
    AUDIO_MANAGER_PLAYBACK_SOURCE_NONE = 0,
    AUDIO_MANAGER_PLAYBACK_SOURCE_RECORDED,
    AUDIO_MANAGER_PLAYBACK_SOURCE_WAV,
    AUDIO_MANAGER_PLAYBACK_SOURCE_PCM16_STREAM,
} audio_manager_playback_source_t;

/**
 * @brief Reason retained with an applied local-source pause.
 *
 * Phase 18.2.1 Prompt 2 implements the owner capability only. The caller that
 * later supplies PTT policy may select PTT, but audio_manager never infers
 * GPIO state or performs automatic resume itself.
 */
typedef enum
{
    AUDIO_MANAGER_PLAYBACK_PAUSE_NONE = 0,
    AUDIO_MANAGER_PLAYBACK_PAUSE_USER,
    AUDIO_MANAGER_PLAYBACK_PAUSE_PTT,
} audio_manager_playback_pause_reason_t;

/** @brief Last bounded playback-control action submitted by a caller. */
typedef enum
{
    AUDIO_MANAGER_PLAYBACK_ACTION_NONE = 0,
    AUDIO_MANAGER_PLAYBACK_ACTION_PAUSE,
    AUDIO_MANAGER_PLAYBACK_ACTION_RESUME,
    AUDIO_MANAGER_PLAYBACK_ACTION_STOP,
    AUDIO_MANAGER_PLAYBACK_ACTION_RESTART,
} audio_manager_playback_action_t;

/**
 * @brief Copied playback-control snapshot; contains no source or driver handle.
 *
 * `position_frames` is the last complete mono frame block submitted to I2S,
 * not an exact speaker/audible cursor. It advances in
 * AUDIO_MANAGER_PLAYBACK_POSITION_GRANULARITY_FRAMES blocks except for natural
 * final completion. A paused WAV/recording resumes from this committed value.
 */
typedef struct
{
    audio_manager_playback_control_state_t state;
    audio_manager_playback_source_t source;
    audio_manager_playback_pause_reason_t pause_reason;
    audio_manager_playback_action_t last_action;
    bool resumable;
    uint32_t generation;
    uint64_t position_frames;
    uint64_t total_frames;
    uint32_t position_granularity_frames;
    esp_err_t last_control_result;
} audio_manager_playback_status_t;

/** @brief Runtime audio settings copied during initialization. */
typedef struct
{
    /** Recording duration used by fixed recording and the stability pipeline. */
    uint32_t record_duration_seconds;

    /**
     * Playback volume in range 0..100. Recorded audio then uses its existing
     * conditioning. WAV and live PCM16 stream audio first use a fixed
     * full-scale PCM16 mapping to the shared output ceiling, then this value
     * applies its linear scale.
     */
    uint32_t playback_volume_percent;
} audio_manager_config_t;

/** @brief Thread-safe snapshot suitable for diagnostics and GUI use. */
typedef struct
{
    /** Current lifecycle or active audio pipeline state. */
    audio_manager_state_t state;

    /** Current runtime playback gain in the inclusive range 0..100. */
    uint32_t playback_volume_percent;

    /** True while the manager has successfully enabled the I2S RX channel. */
    bool capture_i2s_active;

    /** True while the manager has successfully enabled the I2S TX channel. */
    bool playback_i2s_active;

    /** True when a successfully processed retained recording can be replayed. */
    bool recorded_audio_available;

    /** Result of the most recently completed manager audio operation. */
    esp_err_t last_error;

    /** Number of stability cycles that have started. */
    uint32_t cycles_started;

    /** Number of stability cycles that completed successfully. */
    uint32_t cycles_completed;

    /** Number of stability cycles that returned an error. */
    uint32_t cycles_failed;

    /** Number of production fixed/manual recording commands that started. */
    uint32_t recording_started;

    /** Number of production recordings that completed capture and DSP. */
    uint32_t recording_completed;

    /** Number of production recording operations that failed. */
    uint32_t recording_failed;

    /** Number of manual recordings ended by an explicit stop request. */
    uint32_t recording_manual_stopped;

    /** Number of retained-recording playback commands that started. */
    uint32_t recorded_playback_started;

    /** Number of retained-recording playbacks that completed successfully. */
    uint32_t recorded_playback_completed;

    /** Number of retained-recording playbacks that failed. */
    uint32_t recorded_playback_failed;

    /** Number of retained-recording playbacks cancelled cooperatively. */
    uint32_t recorded_playback_cancelled;

    /** Number of WAV commands the manager task began handling. */
    uint32_t wav_playback_started;

    /** Number of WAV commands that reached normal EOF successfully. */
    uint32_t wav_playback_completed;

    /** Number of WAV commands that failed before or during playback. */
    uint32_t wav_playback_failed;

    /** Number of WAV commands cancelled by playback stop or manager stop. */
    uint32_t wav_playback_cancelled;

    /** Number of bounded live PCM16-stream commands accepted by the manager. */
    uint32_t pcm_stream_playback_started;

    /** Number of live PCM16 streams that reached EOS and drained normally. */
    uint32_t pcm_stream_playback_completed;

    /** Number of live PCM16 streams that failed before EOS/normal drain. */
    uint32_t pcm_stream_playback_failed;

    /** Number of live PCM16 streams cancelled or preempted cooperatively. */
    uint32_t pcm_stream_playback_cancelled;

    /** Number of PCM24 samples captured by the most recent recording. */
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
 * Production uses a five-second fixed recording. When the default-off golden
 * stability Kconfig mode is selected, this function returns its configured
 * stress duration; the manager selects that mode's task priority internally.
 */
audio_manager_config_t audio_manager_default_config(void);

/**
 * @brief Initialize audio ownership without starting the manager task.
 *
 * The function copies configuration, allocates one whole-recording PCM24
 * buffer large enough for both fixed recording and the configured manual
 * recording maximum, allocates the DSP workspace plus a bounded live-PCM
 * ingress ring in PSRAM, creates bounded command/lifecycle synchronization,
 * and places MAX98357A data in its safe LOW state. No task or I2S channel
 * remains active when initialization returns.
 * Task context only; do not call from an ISR.
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
 * while an active private reader owns its WAV file and SD lease. A live PCM16
 * producer can only copy packets into a bounded manager-owned PSRAM ring;
 * this task remains its sole I2S/TX consumer. A default-off Kconfig regression
 * mode may run the existing golden stability cycle instead.
 * Its optional continuous WAV-stress coordinator is a separate test task that
 * only polls status and submits commands; it never owns I2S, a WAV stream, or
 * SD access. Active WAV playback uses a private bounded SD-to-PSRAM prefetch
 * worker; the manager remains the only I2S owner and joins that worker before
 * releasing the source. The call waits at most two seconds for manager-task
 * readiness. Serialize start/stop/deinit lifecycle calls in application code;
 * task context only.
 */
esp_err_t audio_manager_start(void);

/**
 * @brief Queue one fixed-duration microphone recording for asynchronous work.
 *
 * The manager must be started and IDLE. Capture duration is the copied
 * audio_manager_config_t.record_duration_seconds value. The manager task owns
 * RX, runs the existing DSP pipeline in place, retains the processed result,
 * and returns to IDLE. The call returns after command acceptance, not after the
 * recording finishes. Use status/callbacks to observe RECORDING, PROCESSING,
 * completion, and recorded_audio_available. Task context only.
 */
esp_err_t audio_manager_record(void);

/**
 * @brief Begin one bounded manual/push-to-talk recording asynchronously.
 *
 * The manager must be started and IDLE. Capture continues until
 * audio_manager_stop_recording() requests a cooperative stop or the compile-
 * time CONFIG_AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS limit is reached,
 * whichever happens first. Successful captured audio is processed through the
 * existing DSP pipeline and retained for audio_manager_play_recorded(). Task
 * context only; do not call from an ISR.
 */
esp_err_t audio_manager_start_recording(void);

/**
 * @brief Request cooperative completion of the active manual recording.
 *
 * This function only sets a bounded control request; the manager task remains
 * the sole I2S owner and observes the request around bounded RX operations.
 * Returns ESP_ERR_INVALID_STATE unless a manual recording is pending/active.
 * Very short recordings that stop before enough samples exist for the DSP are
 * discarded cleanly and leave recorded_audio_available false. Task context
 * only; do not call from an ISR.
 */
esp_err_t audio_manager_stop_recording(void);

/**
 * @brief Queue playback of the most recent valid processed recording.
 *
 * The manager must be started and IDLE and recorded_audio_available must be
 * true. Playback reuses the existing recorded PCM24 -> conditioning -> PCM16
 * TX path; no second playback algorithm or public raw-buffer ownership is
 * introduced. The operation is asynchronous and can be cancelled through
 * audio_manager_stop_playback(). Task context only.
 */
esp_err_t audio_manager_play_recorded(void);

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
 * @brief Request cancellation of pending/active recorded, WAV, or live-stream playback.
 *
 * The caller only sets a bounded cancellation request. The manager task owns
 * source/TX cleanup; WAV's private reader owns its file and checks cancellation
 * between bounded raw reads. Recorded playback checks cancellation between
 * bounded TX blocks, while live PCM16 ingress is flushed before manager-owned
 * TX cleanup. Returns ESP_ERR_INVALID_STATE when no cancellable playback is
 * pending/active. A controlled cancellation completes with IDLE and ESP_OK in
 * status. Task context only.
 */
esp_err_t audio_manager_stop_playback(void);

/**
 * @brief Cooperatively pause a resumable WAV or retained-recording playback.
 *
 * The call records a bounded request and returns after acceptance. `PAUSED` is
 * published only after the manager has stopped TX and committed a 256-frame
 * position. For WAV, the owning task also joins the reader, closes its FILE,
 * releases the SD lease, and frees prefetch buffers before publishing PAUSED.
 * Live PCM16/Xiaozhi streams return ESP_ERR_NOT_SUPPORTED and are unchanged.
 * Repeating pause while PAUSING/PAUSED is idempotent. Task context only.
 */
esp_err_t audio_manager_pause_playback(
    audio_manager_playback_pause_reason_t reason);

/**
 * @brief Cooperatively resume the currently retained paused local source.
 *
 * The call returns after acceptance, not after audible output. WAV resume
 * reopens and revalidates the same manager-copied path, then seeks to the
 * committed frame boundary. Pass zero to address the currently paused source,
 * or pass a previously copied non-zero generation to reject stale auto-resume
 * requests. IDLE/PLAYING/live-stream cases return
 * ESP_ERR_INVALID_STATE or ESP_ERR_NOT_SUPPORTED deterministically. Task
 * context only.
 */
esp_err_t audio_manager_resume_playback(uint32_t expected_generation);

/**
 * @brief Restart the current resumable local source from frame zero.
 *
 * Valid while STARTING, PLAYING, PAUSING, or PAUSED. The existing source
 * identity is reused; callers cannot supply or replace a path. Live PCM16 and
 * IDLE return deterministic errors. The call reports request acceptance only.
 * Task context only.
 */
esp_err_t audio_manager_restart_playback(void);

/**
 * Restart the retained local source only when its generation still matches.
 *
 * This is the stale-event-safe form used by deferred voice/PTT policy. A zero
 * generation has the same current-source semantics as
 * audio_manager_restart_playback(). The request is asynchronous; observe the
 * copied playback status for physical PLAYING evidence.
 */
esp_err_t audio_manager_restart_playback_at_generation(
    uint32_t expected_generation);

/**
 * @brief Copy the current playback-control snapshot without exposing handles.
 *
 * The snapshot is protected by a short critical section and does not wait for
 * physical pause/resume completion. Inspect `state` to distinguish accepted
 * transitions from applied PLAYING/PAUSED state. Task context only.
 */
esp_err_t audio_manager_get_playback_status(
    audio_manager_playback_status_t *status);

/**
 * Set the runtime playback gain in the inclusive range 0..100.
 *
 * The update is lock-free for the sample path: the manager observes the new
 * aligned 32-bit value on a later bounded PCM block. It never restarts I2S or
 * changes playback ownership. Task context only.
 */
esp_err_t audio_manager_set_playback_volume_percent(uint32_t percent);

/** Copy the current runtime playback gain without exposing manager internals. */
esp_err_t audio_manager_get_playback_volume_percent(uint32_t *percent);

/** @brief Convert one playback-control state into stable diagnostic text. */
const char *audio_manager_playback_control_state_to_string(
    audio_manager_playback_control_state_t state);

/** @brief Convert one bounded playback source enum into stable text. */
const char *audio_manager_playback_source_to_string(
    audio_manager_playback_source_t source);

/**
 * @brief Stop the manager task with a finite cooperative shutdown wait.
 *
 * Active production recording/playback is asked to stop cooperatively. Active
 * WAV playback is cancelled by the manager task, which stops I2S then joins
 * the private reader before its source is released; active live PCM16 playback
 * flushes ingress and stops the same manager-owned TX path. When the default-off
 * continuous WAV stress test is selected, this also wakes and joins its non-I2S
 * coordinator task. On success the state becomes INITIALIZED and
 * audio_manager_start() may be called again. The function returns
 * ESP_ERR_TIMEOUT if a task cannot finish its current bounded blocking
 * operation and cleanup before the shutdown deadline; it never force-deletes
 * an owning task. The shutdown wait is five seconds. Do not call this function
 * from the audio manager callback/task itself; serialize it with other
 * lifecycle calls.
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
 * buffer, DSP workspace, live PCM ingress ring, and status mutex after
 * defensive audio cleanup. Task
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
