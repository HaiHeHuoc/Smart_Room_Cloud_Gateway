#include "smart_room_mcp_adapter_internal.h"

#include "audio_manager.h"
#include "voice_assistant_playback_control.h"
#include "xiaozhi_foundation.h"

static bool audio_state_to_foundation(
    audio_manager_playback_control_state_t state,
    xiaozhi_foundation_audio_state_t *mapped)
{
    if (mapped == NULL) {
        return false;
    }
    switch (state) {
        case AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_IDLE;
            return true;
        case AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_STARTING;
            return true;
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_PLAYING;
            return true;
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_PAUSING;
            return true;
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_PAUSED;
            return true;
        case AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_RESUMING;
            return true;
        case AUDIO_MANAGER_PLAYBACK_CONTROL_STOPPING:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_STOPPING;
            return true;
        case AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_STATE_ERROR;
            return true;
        default:
            return false;
    }
}

static bool audio_source_to_foundation(
    audio_manager_playback_source_t source,
    xiaozhi_foundation_audio_source_t *mapped)
{
    if (mapped == NULL) {
        return false;
    }
    switch (source) {
        case AUDIO_MANAGER_PLAYBACK_SOURCE_NONE:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_SOURCE_NONE;
            return true;
        case AUDIO_MANAGER_PLAYBACK_SOURCE_RECORDED:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_SOURCE_RECORDED;
            return true;
        case AUDIO_MANAGER_PLAYBACK_SOURCE_WAV:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_SOURCE_WAV;
            return true;
        case AUDIO_MANAGER_PLAYBACK_SOURCE_PCM16_STREAM:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_SOURCE_LIVE_PCM;
            return true;
        default:
            return false;
    }
}

static bool audio_pause_reason_to_foundation(
    audio_manager_playback_pause_reason_t reason,
    xiaozhi_foundation_audio_pause_reason_t *mapped)
{
    if (mapped == NULL) {
        return false;
    }
    switch (reason) {
        case AUDIO_MANAGER_PLAYBACK_PAUSE_NONE:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_PAUSE_NONE;
            return true;
        case AUDIO_MANAGER_PLAYBACK_PAUSE_USER:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_PAUSE_USER;
            return true;
        case AUDIO_MANAGER_PLAYBACK_PAUSE_PTT:
            *mapped = XIAOZHI_FOUNDATION_AUDIO_PAUSE_PTT_TEMPORARY;
            return true;
        default:
            return false;
    }
}

static esp_err_t audio_copy_snapshot(
    const audio_manager_playback_status_t *source,
    xiaozhi_foundation_audio_playback_snapshot_t *snapshot)
{
    if ((source == NULL) || (snapshot == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = (xiaozhi_foundation_audio_playback_snapshot_t) {0};
    if (!audio_state_to_foundation(source->state, &snapshot->state) ||
        !audio_source_to_foundation(source->source, &snapshot->source_type) ||
        !audio_pause_reason_to_foundation(
            source->pause_reason, &snapshot->pause_reason)) {
        return ESP_ERR_INVALID_STATE;
    }
    snapshot->available = true;
    snapshot->resumable = source->resumable;
    snapshot->generation = source->generation;
    snapshot->position_frames = source->position_frames;
    snapshot->total_frames = source->total_frames;
    snapshot->position_granularity_frames =
        source->position_granularity_frames;
    return ESP_OK;
}

static bool audio_action_to_voice(
    xiaozhi_foundation_audio_action_t action,
    voice_assistant_playback_action_t *mapped)
{
    if (mapped == NULL) {
        return false;
    }
    switch (action) {
        case XIAOZHI_FOUNDATION_AUDIO_ACTION_PAUSE:
            *mapped = VOICE_ASSISTANT_PLAYBACK_ACTION_PAUSE;
            return true;
        case XIAOZHI_FOUNDATION_AUDIO_ACTION_RESUME:
            *mapped = VOICE_ASSISTANT_PLAYBACK_ACTION_RESUME;
            return true;
        case XIAOZHI_FOUNDATION_AUDIO_ACTION_STOP:
            *mapped = VOICE_ASSISTANT_PLAYBACK_ACTION_STOP;
            return true;
        case XIAOZHI_FOUNDATION_AUDIO_ACTION_RESTART:
            *mapped = VOICE_ASSISTANT_PLAYBACK_ACTION_RESTART;
            return true;
        default:
            return false;
    }
}

static xiaozhi_foundation_audio_outcome_t audio_outcome_to_foundation(
    voice_assistant_playback_outcome_t outcome)
{
    switch (outcome) {
        case VOICE_ASSISTANT_PLAYBACK_OUTCOME_SUCCESS:
            return XIAOZHI_FOUNDATION_AUDIO_OUTCOME_SUCCESS;
        case VOICE_ASSISTANT_PLAYBACK_OUTCOME_NO_CURRENT_SOURCE:
            return XIAOZHI_FOUNDATION_AUDIO_OUTCOME_NO_CURRENT_SOURCE;
        case VOICE_ASSISTANT_PLAYBACK_OUTCOME_INVALID_STATE:
            return XIAOZHI_FOUNDATION_AUDIO_OUTCOME_INVALID_STATE;
        case VOICE_ASSISTANT_PLAYBACK_OUTCOME_NON_RESUMABLE_SOURCE:
            return XIAOZHI_FOUNDATION_AUDIO_OUTCOME_NON_RESUMABLE_SOURCE;
        case VOICE_ASSISTANT_PLAYBACK_OUTCOME_STALE_GENERATION:
            return XIAOZHI_FOUNDATION_AUDIO_OUTCOME_STALE_GENERATION;
        case VOICE_ASSISTANT_PLAYBACK_OUTCOME_CONTROL_FAILED:
        default:
            return XIAOZHI_FOUNDATION_AUDIO_OUTCOME_CONTROL_FAILED;
    }
}

static esp_err_t audio_control_provider(
    xiaozhi_foundation_audio_action_t action,
    xiaozhi_foundation_audio_control_result_t *result,
    void *user_context)
{
    (void)user_context;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = (xiaozhi_foundation_audio_control_result_t) {
        .outcome = XIAOZHI_FOUNDATION_AUDIO_OUTCOME_CONTROL_FAILED,
    };

    voice_assistant_playback_action_t voice_action = {0};
    if (!audio_action_to_voice(action, &voice_action)) {
        return ESP_ERR_INVALID_ARG;
    }

    voice_assistant_playback_control_result_t voice_result = {0};
    esp_err_t ret = voice_assistant_playback_control(
        voice_action, &voice_result);
    if (ret != ESP_OK) {
        return ret;
    }
    result->outcome = audio_outcome_to_foundation(voice_result.outcome);
    result->accepted = voice_result.accepted;
    result->physically_applied = voice_result.physically_applied;
    ret = audio_copy_snapshot(&voice_result.playback, &result->playback);
    return ret;
}

static esp_err_t audio_state_provider(
    xiaozhi_foundation_audio_playback_snapshot_t *snapshot,
    void *user_context)
{
    (void)user_context;
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_manager_playback_status_t status = {0};
    const esp_err_t ret = voice_assistant_playback_get_status(&status);
    return (ret == ESP_OK) ? audio_copy_snapshot(&status, snapshot) : ret;
}

esp_err_t smart_room_mcp_audio_playback_register_providers(void)
{
    esp_err_t ret = xiaozhi_foundation_register_audio_control_provider(
        audio_control_provider, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    return xiaozhi_foundation_register_audio_state_provider(
        audio_state_provider, NULL);
}
