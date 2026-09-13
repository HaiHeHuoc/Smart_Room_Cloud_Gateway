#include "voice_assistant_playback_turn_policy.h"

#include <string.h>

void voice_playback_turn_policy_begin(
    voice_playback_turn_policy_t *policy,
    uint32_t ptt_generation,
    uint32_t session_generation,
    uint32_t playback_generation,
    bool has_local_source,
    bool temporary_suspension)
{
    if (policy == NULL) {
        return;
    }
    *policy = (voice_playback_turn_policy_t) {
        .active = true,
        .has_local_source = has_local_source,
        .temporary_suspension = temporary_suspension && has_local_source,
        .ptt_generation = ptt_generation,
        .session_generation = session_generation,
        .playback_generation = has_local_source ? playback_generation : 0U,
    };
}

bool voice_playback_turn_policy_mark_started(
    voice_playback_turn_policy_t *policy,
    uint32_t ptt_generation)
{
    if ((policy == NULL) || !policy->active ||
        (policy->ptt_generation != ptt_generation)) {
        return false;
    }
    policy->turn_started = true;
    return true;
}

bool voice_playback_turn_policy_supersede(
    voice_playback_turn_policy_t *policy,
    uint32_t new_ptt_generation,
    uint32_t session_generation)
{
    if ((policy == NULL) || !policy->active ||
        (new_ptt_generation == 0U)) {
        return false;
    }
    policy->ptt_generation = new_ptt_generation;
    policy->session_generation = session_generation;
    policy->turn_started = false;
    policy->explicit_override = false;
    policy->override_action = VOICE_PLAYBACK_TURN_ACTION_NONE;
    return true;
}

bool voice_playback_turn_policy_set_override(
    voice_playback_turn_policy_t *policy,
    voice_playback_turn_action_t action)
{
    if ((policy == NULL) || !policy->active ||
        !policy->has_local_source ||
        (action < VOICE_PLAYBACK_TURN_ACTION_PAUSE) ||
        (action > VOICE_PLAYBACK_TURN_ACTION_RESTART)) {
        return false;
    }
    policy->explicit_override = true;
    policy->override_action = action;
    return true;
}

bool voice_playback_turn_policy_finish(
    voice_playback_turn_policy_t *policy,
    uint32_t ptt_generation,
    bool require_unstarted,
    voice_playback_turn_action_t *final_action,
    uint32_t *playback_generation)
{
    if ((policy == NULL) || (final_action == NULL) ||
        (playback_generation == NULL) || !policy->active ||
        (policy->ptt_generation != ptt_generation) ||
        (require_unstarted && policy->turn_started)) {
        return false;
    }

    *playback_generation = policy->playback_generation;
    if (policy->explicit_override) {
        *final_action = policy->override_action;
    } else if (policy->temporary_suspension) {
        *final_action = VOICE_PLAYBACK_TURN_ACTION_AUTO_RESUME;
    } else {
        *final_action = VOICE_PLAYBACK_TURN_ACTION_NONE;
    }
    memset(policy, 0, sizeof(*policy));
    return true;
}
