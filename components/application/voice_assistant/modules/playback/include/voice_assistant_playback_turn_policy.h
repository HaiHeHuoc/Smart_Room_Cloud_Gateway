#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    VOICE_PLAYBACK_TURN_ACTION_NONE = 0,
    VOICE_PLAYBACK_TURN_ACTION_PAUSE,
    VOICE_PLAYBACK_TURN_ACTION_RESUME,
    VOICE_PLAYBACK_TURN_ACTION_STOP,
    VOICE_PLAYBACK_TURN_ACTION_RESTART,
    VOICE_PLAYBACK_TURN_ACTION_AUTO_RESUME,
} voice_playback_turn_action_t;

typedef struct {
    bool active;
    bool turn_started;
    bool has_local_source;
    bool temporary_suspension;
    bool explicit_override;
    uint32_t ptt_generation;
    uint32_t session_generation;
    uint32_t playback_generation;
    voice_playback_turn_action_t override_action;
} voice_playback_turn_policy_t;

void voice_playback_turn_policy_begin(
    voice_playback_turn_policy_t *policy,
    uint32_t ptt_generation,
    uint32_t session_generation,
    uint32_t playback_generation,
    bool has_local_source,
    bool temporary_suspension);

bool voice_playback_turn_policy_mark_started(
    voice_playback_turn_policy_t *policy,
    uint32_t ptt_generation);

bool voice_playback_turn_policy_supersede(
    voice_playback_turn_policy_t *policy,
    uint32_t new_ptt_generation,
    uint32_t session_generation);

bool voice_playback_turn_policy_set_override(
    voice_playback_turn_policy_t *policy,
    voice_playback_turn_action_t action);

bool voice_playback_turn_policy_finish(
    voice_playback_turn_policy_t *policy,
    uint32_t ptt_generation,
    bool require_unstarted,
    voice_playback_turn_action_t *final_action,
    uint32_t *playback_generation);
