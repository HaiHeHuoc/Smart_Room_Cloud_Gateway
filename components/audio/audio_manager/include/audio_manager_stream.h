#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Phase-14 voice uplink baseline. */
#define AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ 16000U
#define AUDIO_MANAGER_STREAM_CHANNELS       1U
#define AUDIO_MANAGER_STREAM_FRAME_SAMPLES  256U

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
 * @brief Register or unregister the single Phase-14 live-frame observer.
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
} audio_manager_stream_status_t;

esp_err_t audio_manager_stream_get_status(
    audio_manager_stream_status_t *status);

#ifdef __cplusplus
}
#endif
