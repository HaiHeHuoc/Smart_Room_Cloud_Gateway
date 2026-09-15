#pragma once

#include "audio_manager.h"

/** Private owner-side events used to keep control transitions deterministic. */
typedef enum
{
    AUDIO_MANAGER_PLAYBACK_EVENT_BEGIN = 0,
    AUDIO_MANAGER_PLAYBACK_EVENT_PLAY_APPLIED,
    AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_REQUESTED,
    AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_APPLIED,
    AUDIO_MANAGER_PLAYBACK_EVENT_RESUME_REQUESTED,
    AUDIO_MANAGER_PLAYBACK_EVENT_RESTART_REQUESTED,
    AUDIO_MANAGER_PLAYBACK_EVENT_STOP_REQUESTED,
    AUDIO_MANAGER_PLAYBACK_EVENT_FINISHED,
    AUDIO_MANAGER_PLAYBACK_EVENT_FAILED,
    AUDIO_MANAGER_PLAYBACK_EVENT_RECOVERED,
} audio_manager_playback_control_event_t;

/**
 * Evaluate one playback-control transition without touching runtime state.
 * ESP_OK includes idempotent PAUSE/STOP cases; invalid source/state policy is
 * enforced by the public audio_manager facade before this owner transition.
 */
esp_err_t audio_manager_playback_control_transition(
    audio_manager_playback_control_state_t current,
    audio_manager_playback_control_event_t event,
    audio_manager_playback_control_state_t *next);
