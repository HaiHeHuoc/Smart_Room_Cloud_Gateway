#include "voice_assistant_ptt_owner_policy.h"

#include <string.h>

bool voice_assistant_ptt_owner_policy_reserve(
    voice_assistant_ptt_owner_policy_t *policy,
    voice_assistant_ptt_source_t source,
    uint32_t client_id,
    uint32_t ptt_generation,
    int64_t now_us)
{
    if ((policy == NULL) ||
        ((source != VOICE_ASSISTANT_PTT_SOURCE_GPIO) &&
         (source != VOICE_ASSISTANT_PTT_SOURCE_WEB)) ||
        ((source == VOICE_ASSISTANT_PTT_SOURCE_WEB) && (client_id == 0U)) ||
        (ptt_generation == 0U) || (now_us < 0) || policy->pressed) {
        return false;
    }

    policy->source = source;
    policy->client_id = (source == VOICE_ASSISTANT_PTT_SOURCE_WEB) ? client_id : 0U;
    policy->ptt_generation = ptt_generation;
    policy->pressed = true;
    policy->web_lease_expires_at_us =
        (source == VOICE_ASSISTANT_PTT_SOURCE_WEB)
            ? now_us + ((int64_t)VOICE_ASSISTANT_PTT_WEB_LEASE_TIMEOUT_MS * 1000LL)
            : 0;
    policy->web_lease_expiring = false;
    return true;
}

bool voice_assistant_ptt_owner_policy_matches(
    const voice_assistant_ptt_owner_policy_t *policy,
    voice_assistant_ptt_source_t source,
    uint32_t client_id,
    uint32_t ptt_generation)
{
    return (policy != NULL) && policy->pressed &&
           (policy->source == source) &&
           (policy->ptt_generation == ptt_generation) &&
           ((source != VOICE_ASSISTANT_PTT_SOURCE_WEB) ||
            ((client_id != 0U) && (policy->client_id == client_id)));
}

bool voice_assistant_ptt_owner_policy_refresh_web_lease(
    voice_assistant_ptt_owner_policy_t *policy,
    uint32_t client_id,
    uint32_t ptt_generation,
    int64_t now_us)
{
    if (!voice_assistant_ptt_owner_policy_matches(
            policy, VOICE_ASSISTANT_PTT_SOURCE_WEB, client_id, ptt_generation) ||
        (now_us < 0) || policy->web_lease_expiring) {
        return false;
    }

    /* Do not let a keepalive arriving just after expiry resurrect a turn
     * before the policy worker performs its bounded release. */
    if ((policy->web_lease_expires_at_us > 0) &&
        (now_us >= policy->web_lease_expires_at_us)) {
        policy->web_lease_expiring = true;
        return false;
    }

    policy->web_lease_expires_at_us =
        now_us + ((int64_t)VOICE_ASSISTANT_PTT_WEB_LEASE_TIMEOUT_MS * 1000LL);
    return true;
}

bool voice_assistant_ptt_owner_policy_web_lease_expired(
    voice_assistant_ptt_owner_policy_t *policy,
    int64_t now_us,
    uint32_t *client_id,
    uint32_t *ptt_generation)
{
    if ((policy == NULL) || (now_us < 0) || !policy->pressed ||
        (policy->source != VOICE_ASSISTANT_PTT_SOURCE_WEB) ||
        (policy->web_lease_expires_at_us <= 0) ||
        (now_us < policy->web_lease_expires_at_us)) {
        return false;
    }

    policy->web_lease_expiring = true;
    if (client_id != NULL) {
        *client_id = policy->client_id;
    }
    if (ptt_generation != NULL) {
        *ptt_generation = policy->ptt_generation;
    }
    return true;
}

void voice_assistant_ptt_owner_policy_release(
    voice_assistant_ptt_owner_policy_t *policy)
{
    if (policy != NULL) {
        memset(policy, 0, sizeof(*policy));
        policy->source = VOICE_ASSISTANT_PTT_SOURCE_NONE;
    }
}
