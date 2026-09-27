#include "voice_assistant_audio_adapter.h"

#include "voice_assistant.h"
#include "voice_assistant_audio_arbitration_bridge.h"

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

    /* audio_manager is also the owner of wake-word and local-playback I2S.
     * The Xiaozhi UI must show RECORDING/PLAYBACK only for its own reserved
     * resource, otherwise opening the network makes passive wake listening
     * look like a user speech turn. These atomic ownership snapshots keep the
     * status callback non-blocking. */
    const bool capture_active = status->capture_i2s_active &&
        voice_assistant_audio_capture_owned();
    const bool playback_active = status->playback_i2s_active &&
        voice_assistant_audio_playback_owned();
    voice_assistant_audio_state_t state = voice_assistant_map_audio_state(status->state);
    if ((status->state != AUDIO_MANAGER_STATE_ERROR) &&
        !capture_active && !playback_active) {
        state = VOICE_ASSISTANT_AUDIO_IDLE;
    }

    const voice_assistant_audio_status_t voice_status = {
        .state = state,
        .capture_active = capture_active,
        .playback_active = playback_active,
        .last_error = status->last_error,
    };

    return voice_assistant_notify_audio_status(&voice_status);
}
