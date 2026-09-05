#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Private scalar view for the playback arbiter. It never exposes PCM storage
 * or permits a caller to touch I2S. */
typedef struct {
    bool active;
    bool ended;
    bool aborted;
    bool tx_started;
    uint32_t generation;
    size_t queued_samples;
    uint64_t accepted_samples;
    uint64_t played_samples;
    uint32_t high_water_samples;
    uint32_t full_count;
    /** Number of distinct post-start ingress-empty episodes. */
    uint32_t starvation_count;
} audio_manager_pcm_stream_status_t;

/** Reserve the single manager-owned PCM16 ring for a request generation. */
esp_err_t audio_manager_pcm_stream_prepare(uint32_t generation);

/** Queue the manager-owned playback command for a prepared generation. */
esp_err_t audio_manager_pcm_stream_start(uint32_t generation);

/** Copy one bounded 16 kHz mono PCM16 packet into the manager-owned ring. */
esp_err_t audio_manager_pcm_stream_write(
    uint32_t generation,
    const int16_t *samples,
    size_t sample_count);

/** Mark ingress EOF; queued PCM remains available for the manager to drain. */
esp_err_t audio_manager_pcm_stream_finish(uint32_t generation);

/** Flush a prepared/active generation and wake the I2S-owning manager task. */
esp_err_t audio_manager_pcm_stream_abort(uint32_t generation);

/** Copy stream metrics/state for arbitration only. */
esp_err_t audio_manager_pcm_stream_get_status(
    uint32_t generation,
    audio_manager_pcm_stream_status_t *status);

#ifdef __cplusplus
}
#endif
