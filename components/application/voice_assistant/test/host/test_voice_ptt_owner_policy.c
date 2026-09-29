#include <stdbool.h>
#include <stdio.h>

#include "voice_assistant_ptt_owner_policy.h"

static bool test_web_start_keepalive_and_stop(void)
{
    voice_assistant_ptt_owner_policy_t policy = {
        .source = VOICE_ASSISTANT_PTT_SOURCE_NONE,
    };
    const int64_t started_at_us = 1000000;

    if (!voice_assistant_ptt_owner_policy_reserve(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 77U, 42U, started_at_us) ||
        !voice_assistant_ptt_owner_policy_matches(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 77U, 42U) ||
        !voice_assistant_ptt_owner_policy_refresh_web_lease(
            &policy, 77U, 42U, started_at_us + 1000000)) {
        return false;
    }

    uint32_t client_id = 0U;
    uint32_t generation = 0U;
    if (voice_assistant_ptt_owner_policy_web_lease_expired(
            &policy, started_at_us + 8000000, &client_id, &generation) ||
        !voice_assistant_ptt_owner_policy_web_lease_expired(
            &policy, started_at_us + 9000000, &client_id, &generation) ||
        (client_id != 77U) || (generation != 42U) ||
        voice_assistant_ptt_owner_policy_refresh_web_lease(
            &policy, 77U, 42U, started_at_us + 9000001)) {
        return false;
    }

    voice_assistant_ptt_owner_policy_release(&policy);
    /* A heartbeat received at/after the deadline cannot revive the old turn
     * while the PTT worker is about to queue its release. */
    if (!voice_assistant_ptt_owner_policy_reserve(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 78U, 43U, started_at_us) ||
        voice_assistant_ptt_owner_policy_refresh_web_lease(
            &policy, 78U, 43U,
            started_at_us + ((int64_t)VOICE_ASSISTANT_PTT_WEB_LEASE_TIMEOUT_MS * 1000LL)) ||
        !voice_assistant_ptt_owner_policy_web_lease_expired(
            &policy,
            started_at_us + ((int64_t)VOICE_ASSISTANT_PTT_WEB_LEASE_TIMEOUT_MS * 1000LL),
            &client_id, &generation)) {
        return false;
    }
    voice_assistant_ptt_owner_policy_release(&policy);
    return (policy.source == VOICE_ASSISTANT_PTT_SOURCE_NONE) && !policy.pressed;
}

static bool test_stale_and_competing_commands(void)
{
    voice_assistant_ptt_owner_policy_t policy = {
        .source = VOICE_ASSISTANT_PTT_SOURCE_NONE,
    };

    if (!voice_assistant_ptt_owner_policy_reserve(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_GPIO, 0U, 10U, 10)) {
        return false;
    }
    /* GPIO active + Web start must remain BUSY. */
    if (voice_assistant_ptt_owner_policy_reserve(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 1U, 11U, 20)) {
        return false;
    }
    voice_assistant_ptt_owner_policy_release(&policy);

    if (!voice_assistant_ptt_owner_policy_reserve(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 2U, 11U, 30) ||
        /* Web active + GPIO press must remain BUSY. */
        voice_assistant_ptt_owner_policy_reserve(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_GPIO, 0U, 12U, 40) ||
        /* A second Web client cannot own the current turn. */
        voice_assistant_ptt_owner_policy_matches(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 3U, 11U)) {
        return false;
    }
    voice_assistant_ptt_owner_policy_release(&policy);

    if (!voice_assistant_ptt_owner_policy_reserve(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 2U, 12U, 50) ||
        /* A delayed STOP/heartbeat from generation 11 cannot affect 12. */
        voice_assistant_ptt_owner_policy_matches(
            &policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, 2U, 11U) ||
        voice_assistant_ptt_owner_policy_refresh_web_lease(
            &policy, 2U, 11U, 60) ||
        !voice_assistant_ptt_owner_policy_refresh_web_lease(
            &policy, 2U, 12U, 60)) {
        return false;
    }

    voice_assistant_ptt_owner_policy_release(&policy);
    return voice_assistant_ptt_owner_policy_reserve(
        &policy, VOICE_ASSISTANT_PTT_SOURCE_GPIO, 0U, 13U, 70);
}

int main(void)
{
    const bool ok = test_web_start_keepalive_and_stop() &&
                    test_stale_and_competing_commands();
    puts(ok ? "voice PTT owner/lease policy: PASS" :
              "voice PTT owner/lease policy: FAIL");
    return ok ? 0 : 1;
}
