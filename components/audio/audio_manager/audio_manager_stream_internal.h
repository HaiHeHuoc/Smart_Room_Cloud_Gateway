#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Private audio-manager producer hook.
 * Caller must pass a temporary mono PCM16 block owned by audio_manager task.
 */
esp_err_t audio_manager_stream_publish_internal(
    const int16_t *samples,
    size_t sample_count);
