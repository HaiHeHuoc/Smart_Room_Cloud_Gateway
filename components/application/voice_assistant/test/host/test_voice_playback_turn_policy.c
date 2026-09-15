#include <stdbool.h>
#include <stdio.h>

#include "voice_assistant_playback_turn_policy.h"

static bool expect_finish(
    voice_playback_turn_policy_t *policy,
    uint32_t ptt_generation,
    bool require_unstarted,
    voice_playback_turn_action_t expected_action,
    uint32_t expected_playback_generation)
{
    voice_playback_turn_action_t action = VOICE_PLAYBACK_TURN_ACTION_NONE;
    uint32_t playback_generation = 0U;
    return voice_playback_turn_policy_finish(
               policy,
               ptt_generation,
               require_unstarted,
               &action,
               &playback_generation) &&
           (action == expected_action) &&
           (playback_generation == expected_playback_generation) &&
           !policy->active;
}

int main(void)
{
    bool ok = true;
    voice_playback_turn_policy_t policy = {0};

    /* PTT suspension eligible: the retained local generation auto-resumes. */
    voice_playback_turn_policy_begin(&policy, 10U, 3U, 41U, true, true);
    ok &= policy.active && policy.has_local_source &&
          policy.temporary_suspension;
    ok &= voice_playback_turn_policy_mark_started(&policy, 10U);
    ok &= expect_finish(&policy, 10U, false,
                        VOICE_PLAYBACK_TURN_ACTION_AUTO_RESUME, 41U);

    /* PTT suspension not needed: a normal voice turn has no final action. */
    voice_playback_turn_policy_begin(&policy, 11U, 3U, 0U, false, false);
    ok &= expect_finish(&policy, 11U, false,
                        VOICE_PLAYBACK_TURN_ACTION_NONE, 0U);

    /* A source paused by the user must stay paused after an unrelated turn. */
    voice_playback_turn_policy_begin(&policy, 12U, 3U, 42U, true, false);
    ok &= expect_finish(&policy, 12U, false,
                        VOICE_PLAYBACK_TURN_ACTION_NONE, 42U);

    const voice_playback_turn_action_t overrides[] = {
        VOICE_PLAYBACK_TURN_ACTION_STOP,
        VOICE_PLAYBACK_TURN_ACTION_PAUSE,
        VOICE_PLAYBACK_TURN_ACTION_RESUME,
        VOICE_PLAYBACK_TURN_ACTION_RESTART,
    };
    for (size_t i = 0U; i < (sizeof(overrides) / sizeof(overrides[0])); ++i) {
        voice_playback_turn_policy_begin(
            &policy, (uint32_t)(20U + i), 4U, 50U, true, true);
        ok &= voice_playback_turn_policy_set_override(&policy, overrides[i]);
        ok &= expect_finish(&policy, (uint32_t)(20U + i), false,
                            overrides[i], 50U);
    }

    /* Last explicit command in one turn owns the final state. */
    voice_playback_turn_policy_begin(&policy, 30U, 5U, 60U, true, true);
    ok &= voice_playback_turn_policy_set_override(
        &policy, VOICE_PLAYBACK_TURN_ACTION_RESUME);
    ok &= voice_playback_turn_policy_set_override(
        &policy, VOICE_PLAYBACK_TURN_ACTION_STOP);
    ok &= expect_finish(&policy, 30U, false,
                        VOICE_PLAYBACK_TURN_ACTION_STOP, 60U);

    /* Fast release restores only a turn that never crossed its start marker. */
    voice_playback_turn_policy_begin(&policy, 31U, 5U, 61U, true, true);
    ok &= expect_finish(&policy, 31U, true,
                        VOICE_PLAYBACK_TURN_ACTION_AUTO_RESUME, 61U);
    voice_playback_turn_policy_begin(&policy, 32U, 5U, 62U, true, true);
    ok &= voice_playback_turn_policy_mark_started(&policy, 32U);
    voice_playback_turn_action_t action = VOICE_PLAYBACK_TURN_ACTION_NONE;
    uint32_t generation = 0U;
    ok &= !voice_playback_turn_policy_finish(
        &policy, 32U, true, &action, &generation);
    ok &= policy.active;
    ok &= expect_finish(&policy, 32U, false,
                        VOICE_PLAYBACK_TURN_ACTION_AUTO_RESUME, 62U);

    /* Stale terminal events cannot consume a newer PTT transaction. */
    voice_playback_turn_policy_begin(&policy, 40U, 6U, 70U, true, true);
    ok &= !voice_playback_turn_policy_finish(
        &policy, 39U, false, &action, &generation);
    ok &= policy.active;

    /* TTS interruption transfers the same suspended source to the new press. */
    ok &= voice_playback_turn_policy_supersede(&policy, 41U, 6U);
    ok &= !voice_playback_turn_policy_finish(
        &policy, 40U, false, &action, &generation);
    ok &= expect_finish(&policy, 41U, false,
                        VOICE_PLAYBACK_TURN_ACTION_AUTO_RESUME, 70U);

    puts(ok ? "voice playback turn policy: PASS" :
              "voice playback turn policy: FAIL");
    return ok ? 0 : 1;
}
