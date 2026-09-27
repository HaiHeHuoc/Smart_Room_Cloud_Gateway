#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Voice uplink stream contract. */
#define AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ 16000U
#define AUDIO_MANAGER_STREAM_CHANNELS       1U
#define AUDIO_MANAGER_STREAM_FRAME_SAMPLES  256U
/* Four copied frames provide a 64 ms local-consumer jitter budget without
 * retaining multi-second microphone audio. The existing PTT consumer keeps
 * ownership of its independent 16-frame queue in voice_assistant. */
#define AUDIO_MANAGER_STREAM_LOCAL_MONITOR_QUEUE_LENGTH 4U

/**
 * @brief Borrowed mono PCM16 frame produced by the audio-manager task.
 *
 * `samples` is valid only for the duration of the callback. Consumers must
 * copy the samples before returning if they need to retain them. No DMA/I2S
 * buffer or manager-owned PSRAM pointer crosses this boundary.
 */
typedef struct
{
    const int16_t *samples;
    size_t sample_count;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint32_t stream_generation;
    uint64_t frame_sequence;
} audio_manager_stream_frame_t;

/**
 * @brief One queue-owned local-monitor PCM16 frame.
 *
 * The queue copies this complete object. `samples` never aliases an I2S DMA
 * buffer, the conversion tap, or a caller-owned buffer. A local-monitor
 * reader owns its stack/local destination after
 * audio_manager_stream_local_monitor_receive() returns.
 */
typedef struct
{
    uint64_t frame_sequence;
    uint32_t sample_rate_hz;
    uint16_t sample_count;
    uint8_t channels;
    uint8_t reserved;
    int16_t samples[AUDIO_MANAGER_STREAM_FRAME_SAMPLES];
} audio_manager_stream_local_monitor_frame_t;

/** Copied local-monitor diagnostics. No queue, DMA, or I2S handle escapes. */
typedef struct
{
    bool registered;
    bool enabled;
    /** A WakeNet-owned hands-free turn may retain VAD input while PTT streams. */
    bool allow_during_ptt;
    uint64_t frames_published;
    uint64_t frames_delivered;
    uint64_t frames_dropped_queue_full;
    uint64_t frames_suppressed_ptt;
    uint64_t sequence_gap_count;
    uint32_t queue_peak_depth;
} audio_manager_stream_local_monitor_status_t;

/**
 * @brief Non-blocking live-capture frame observer.
 *
 * Runs in the audio-manager task context, never ISR context. The callback must
 * return promptly and must not call blocking networking, LVGL, I2S lifecycle,
 * or audio_manager lifecycle APIs. A typical consumer copies/enqueues the frame
 * into its own bounded transport queue and returns.
 */
typedef void (*audio_manager_stream_frame_callback_t)(
    const audio_manager_stream_frame_t *frame,
    void *user_context);

/**
 * @brief Register or unregister the single live-frame observer.
 *
 * Passing NULL unregisters the observer. Registration alone never starts I2S
 * or recording. The audio manager remains the sole microphone/I2S owner.
 */
esp_err_t audio_manager_stream_register_callback(
    audio_manager_stream_frame_callback_t callback,
    void *user_context);

/**
 * @brief Arm one logical live-stream generation.
 *
 * The caller supplies a non-zero generation owned by the higher-level voice
 * transaction. Arming does not start capture; it only authorizes frame
 * publication when the audio-manager capture producer is active.
 */
esp_err_t audio_manager_stream_arm(uint32_t stream_generation);

/**
 * @brief Revoke live-frame publication for the current generation.
 *
 * This does not itself stop I2S. Capture lifecycle remains controlled by
 * audio_manager_start_recording()/audio_manager_stop_recording().
 */
esp_err_t audio_manager_stream_disarm(uint32_t stream_generation);

/** @brief Copy current stream contract state for diagnostics/tests. */
typedef struct
{
    bool armed;
    uint32_t stream_generation;
    uint64_t frames_published;
    uint64_t samples_published;
    uint64_t frames_dropped_no_callback;
    audio_manager_stream_local_monitor_status_t local_monitor;
} audio_manager_stream_status_t;

esp_err_t audio_manager_stream_get_status(
    audio_manager_stream_status_t *status);

/**
 * @brief Reserve the sole bounded copied PCM queue for a future local monitor.
 *
 * At most one local monitor may be registered. Registration allocates one
 * fixed PSRAM queue and never starts RX/I2S or sends audio externally.
 * Registering twice returns ESP_ERR_INVALID_STATE.
 */
esp_err_t audio_manager_stream_local_monitor_register(void);

/**
 * @brief Disable and release the local-monitor registration.
 *
 * This is rejected while the local feed or a PTT stream is active. Queue
 * storage remains manager-owned until manager teardown, preventing producer
 * lifetime races and avoiding allocation churn.
 */
esp_err_t audio_manager_stream_local_monitor_unregister(void);

/**
 * @brief Enable or disable copied local-monitor publication.
 *
 * Enabling resets stale queued PCM but does not start microphone capture.
 * While an armed PTT generation is live, local frames are deliberately
 * suppressed so wake processing cannot compete with live Xiaozhi capture.
 * The future consumer must quiesce its reader before disabling/unregistering.
 */
esp_err_t audio_manager_stream_local_monitor_set_enabled(bool enabled);

/**
 * @brief Allow the registered local monitor to receive copied frames during
 * an armed PTT stream.
 *
 * Disabled by default. The WakeNet owner enables this only for its current
 * hands-free VAD handoff, then clears it before the response phase. It never
 * changes I2S ownership or the existing PTT callback ordering.
 */
esp_err_t audio_manager_stream_local_monitor_set_allow_during_ptt(bool allowed);


/**
 * @brief Receive one copied local-monitor frame in consumer task context.
 *
 * `timeout_ms` is a bounded wait owned by the consumer, never by the
 * audio-manager producer. ESP_ERR_TIMEOUT means no copied frame arrived.
 */
esp_err_t audio_manager_stream_local_monitor_receive(
    audio_manager_stream_local_monitor_frame_t *frame,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
