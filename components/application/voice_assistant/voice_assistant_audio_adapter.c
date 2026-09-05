#include "voice_assistant_audio_adapter.h"

#include "voice_assistant.h"

static voice_assistant_audio_state_t voice_assistant_map_audio_state(
    audio_manager_state_t state)
{
    switch (state) {
        case AUDIO_MANAGER_STATE_INITIALIZED:
            return VOICE_ASSISTANT_AUDIO_INITIALIZED;
        case AUDIO_MANAGER_STATE_IDLE:
            return VOICE_ASSISTANT_AUDIO_IDLE;
        case AUDIO_MANAGER_STATE_RECORDING:
            return VOICE_ASSISTANT_AUDIO_RECORDING;
        case AUDIO_MANAGER_STATE_PROCESSING:
            return VOICE_ASSISTANT_AUDIO_PROCESSING;
        case AUDIO_MANAGER_STATE_PLAYBACK:
            return VOICE_ASSISTANT_AUDIO_PLAYBACK;
        case AUDIO_MANAGER_STATE_ERROR:
            return VOICE_ASSISTANT_AUDIO_ERROR;
        case AUDIO_MANAGER_STATE_UNINITIALIZED:
        default:
            return VOICE_ASSISTANT_AUDIO_UNAVAILABLE;
    }
}

esp_err_t voice_assistant_audio_adapter_post(
    const audio_manager_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const voice_assistant_audio_status_t voice_status = {
        .state = voice_assistant_map_audio_state(status->state),
        .capture_active = status->capture_i2s_active,
        .playback_active = status->playback_i2s_active,
        .last_error = status->last_error,
    };

    return voice_assistant_notify_audio_status(&voice_status);
}
