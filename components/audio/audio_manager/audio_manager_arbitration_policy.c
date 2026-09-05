#include "audio_manager_arbitration_policy.h"

audio_manager_policy_decision_t audio_manager_arbitration_policy_decide(
    const audio_manager_request_t *incoming,
    const audio_manager_policy_owner_t *owner)
{
    if ((incoming == NULL) || (audio_manager_request_validate(incoming) != ESP_OK)) {
        return AUDIO_MANAGER_POLICY_REJECT;
    }

    if ((owner == NULL) || !owner->current_valid) {
        return AUDIO_MANAGER_POLICY_GRANT;
    }

    if (audio_manager_request_validate(&owner->current) != ESP_OK) {
        return AUDIO_MANAGER_POLICY_REJECT;
    }

    switch (incoming->busy_policy) {
        case AUDIO_MANAGER_BUSY_REJECT:
            return AUDIO_MANAGER_POLICY_REJECT;

        case AUDIO_MANAGER_BUSY_QUEUE:
            return AUDIO_MANAGER_POLICY_WAIT;

        case AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY:
            if (owner->current.interruptible &&
                (incoming->priority > owner->current.priority)) {
                return AUDIO_MANAGER_POLICY_PREEMPT;
            }
            return AUDIO_MANAGER_POLICY_REJECT;

        default:
            return AUDIO_MANAGER_POLICY_REJECT;
    }
}

const char *audio_manager_policy_decision_to_string(
    audio_manager_policy_decision_t decision)
{
    switch (decision) {
        case AUDIO_MANAGER_POLICY_GRANT: return "GRANT";
        case AUDIO_MANAGER_POLICY_WAIT: return "WAIT";
        case AUDIO_MANAGER_POLICY_REJECT: return "REJECT";
        case AUDIO_MANAGER_POLICY_PREEMPT: return "PREEMPT";
        default: return "UNKNOWN";
    }
}
