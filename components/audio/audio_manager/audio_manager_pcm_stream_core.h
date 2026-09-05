#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One Xiaozhi Opus decoder output is 60 ms at 16 kHz. Keep the ingress
 * contract explicit so the bounded ring can reject an accidental bulk write
 * rather than partially accepting it. */
#define AUDIO_MANAGER_PCM_STREAM_MAX_WRITE_SAMPLES 960U

/**
 * @brief Single-producer/single-consumer PCM16 FIFO state.
 *
 * The caller owns synchronization and storage. This module only performs
 * bounded copy/ordering/state transitions, which keeps it host-testable and
 * prevents a producer from retaining a decoder-owned buffer.
 */
typedef struct {
    int16_t *storage;
    size_t capacity_samples;
    size_t read_index;
    size_t write_index;
    size_t queued_samples;
    uint32_t generation;
    bool active;
    bool ended;
    bool aborted;
    uint64_t accepted_samples;
    uint64_t consumed_samples;
    uint32_t high_water_samples;
    uint32_t full_count;
    /** Number of distinct post-start ingress-empty episodes. */
    uint32_t starvation_count;
} audio_manager_pcm_stream_core_t;

/** Attach caller-owned PCM16 storage. The core starts inactive. */
esp_err_t audio_manager_pcm_stream_core_init(
    audio_manager_pcm_stream_core_t *core,
    int16_t *storage,
    size_t capacity_samples);

/** Start a fresh generation and discard every sample from the prior one. */
esp_err_t audio_manager_pcm_stream_core_prepare(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation);

/**
 * Copy one complete PCM16 packet atomically. No partial packet is accepted;
 * ESP_ERR_TIMEOUT means the ring lacks room for the entire packet.
 */
esp_err_t audio_manager_pcm_stream_core_write(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation,
    const int16_t *samples,
    size_t sample_count);

/** Copy up to capacity samples in FIFO order. ESP_ERR_NOT_FOUND means empty. */
esp_err_t audio_manager_pcm_stream_core_read(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation,
    int16_t *samples,
    size_t capacity_samples,
    size_t *samples_read);

/** Mark ingress EOF. Previously queued samples remain available to drain. */
esp_err_t audio_manager_pcm_stream_core_finish(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation);

/** Flush queued samples and reject future writes until close/prepare. */
esp_err_t audio_manager_pcm_stream_core_abort(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation);

/** Release the active session after the I2S owner has completed cleanup. */
esp_err_t audio_manager_pcm_stream_core_close(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation);

/** Copy scalar session diagnostics without exposing storage. */
esp_err_t audio_manager_pcm_stream_core_get(
    const audio_manager_pcm_stream_core_t *core,
    uint32_t generation,
    audio_manager_pcm_stream_core_t *snapshot);

#ifdef __cplusplus
}
#endif
