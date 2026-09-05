#pragma once

#include "esp_err.h"
#include "audio_manager.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Translate and queue one copied audio_manager status for voice logic.
 *
 * This adapter exists so application composition can fan out the existing
 * audio_manager callback without registering a second owner callback. It never
 * calls I2S, starts/stops audio, or retains @p status.
 *
 * @return ESP_OK when queued, ESP_ERR_INVALID_ARG for NULL/invalid input,
 *         ESP_ERR_INVALID_STATE before voice_assistant_start(), or
 *         ESP_ERR_TIMEOUT when the bounded voice command queue is full.
 */
esp_err_t voice_assistant_audio_adapter_post(
    const audio_manager_status_t *status);

#ifdef __cplusplus
}
#endif
