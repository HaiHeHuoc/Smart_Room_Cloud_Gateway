#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define VOICE_ASSISTANT_OPUS_SAMPLE_RATE_HZ       16000U
#define VOICE_ASSISTANT_OPUS_CHANNELS             1U
#define VOICE_ASSISTANT_OPUS_FRAME_DURATION_MS    60U
#define VOICE_ASSISTANT_OPUS_PCM_SAMPLES          960U
#define VOICE_ASSISTANT_OPUS_PCM_BYTES            \
    (VOICE_ASSISTANT_OPUS_PCM_SAMPLES * sizeof(int16_t))
#define VOICE_ASSISTANT_OPUS_MAX_PACKET_BYTES     2048U

/** Initialize the uplink encoder. Call before starting the uplink task. */
esp_err_t voice_assistant_opus_encoder_init(void);

/** Reset per-turn encoder state. Called only by the uplink task. */
esp_err_t voice_assistant_opus_encoder_reset(void);

/** Encode exactly one 60-ms PCM16 mono frame into one Opus packet. */
esp_err_t voice_assistant_opus_encode(
    const int16_t *pcm,
    size_t sample_count,
    uint8_t *packet,
    size_t packet_capacity,
    size_t *packet_size);

/** Initialize the downlink decoder. Call before starting the downlink task. */
esp_err_t voice_assistant_opus_decoder_init(void);

/** Reset per-response decoder state. Called only by the downlink task. */
esp_err_t voice_assistant_opus_decoder_reset(void);

/** Decode one Opus packet into bounded PCM16 mono output. */
esp_err_t voice_assistant_opus_decode(
    const uint8_t *packet,
    size_t packet_size,
    int16_t *pcm,
    size_t pcm_capacity_samples,
    size_t *pcm_samples);
