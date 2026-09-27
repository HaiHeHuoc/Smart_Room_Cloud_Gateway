#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Private audio-manager producer hook for one temporary mono PCM16 block. */
esp_err_t audio_manager_stream_publish_internal(
    const int16_t *samples,
    size_t sample_count);

/** Reset/enable the raw-slot conversion tap for one armed live stream. */
void audio_manager_stream_tap_arm(void);

/** Disable the conversion tap immediately; partial frames are discarded. */
void audio_manager_stream_tap_disarm(void);

/** Enable/disable the independent local-monitor conversion feed. */
void audio_manager_stream_tap_set_local_monitor_enabled(bool enabled);

/**
 * It preserves audio_dsp_convert_raw_slot_to_pcm24() semantics and additionally
 * publishes selected live microphone samples while a stream is armed.
 */
int32_t audio_manager_stream_convert_raw_slot_to_pcm24(int32_t raw_slot);
