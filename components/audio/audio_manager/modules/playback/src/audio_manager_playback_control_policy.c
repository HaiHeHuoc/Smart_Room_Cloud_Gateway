#include "audio_manager_playback_control_policy.h"

esp_err_t audio_manager_playback_control_transition(
    audio_manager_playback_control_state_t current,
    audio_manager_playback_control_event_t event,
    audio_manager_playback_control_state_t *next)
{
    if (next == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    audio_manager_playback_control_state_t result = current;
    switch (event)
    {
        case AUDIO_MANAGER_PLAYBACK_EVENT_BEGIN:
            if (current != AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_PLAY_APPLIED:
            if ((current != AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING) &&
                (current != AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING))
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_REQUESTED:
            if ((current == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING) ||
                (current == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED))
            {
                break;
            }
            if ((current != AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING) &&
                (current != AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING))
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_APPLIED:
            if (current != AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING)
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_RESUME_REQUESTED:
            if (current != AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED)
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_RESTART_REQUESTED:
            if ((current == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED) ||
                (current == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING))
            {
                result = AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING;
                break;
            }
            if ((current != AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING) &&
                (current != AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING) &&
                (current != AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING))
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_STOP_REQUESTED:
            if (current == AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)
            {
                break;
            }
            if (current == AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR)
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_STOPPING;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_FINISHED:
            if (current == AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)
            {
                break;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_FAILED:
            if (current == AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR;
            break;

        case AUDIO_MANAGER_PLAYBACK_EVENT_RECOVERED:
            if (current != AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR)
            {
                return ESP_ERR_INVALID_STATE;
            }
            result = AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE;
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    *next = result;
    return ESP_OK;
}
