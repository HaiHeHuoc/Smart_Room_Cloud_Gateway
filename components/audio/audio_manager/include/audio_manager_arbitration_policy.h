#pragma once

#include <stdbool.h>

#include "audio_manager_request.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_MANAGER_POLICY_GRANT = 0,
    AUDIO_MANAGER_POLICY_WAIT,
    AUDIO_MANAGER_POLICY_REJECT,
    AUDIO_MANAGER_POLICY_PREEMPT,
} audio_manager_policy_decision_t;

typedef struct {
    bool current_valid;
    audio_manager_request_t current;
} audio_manager_policy_owner_t;

/**
 * Decide how an incoming request relates to a known current owner.
 *
 * Rules are deterministic and resource-agnostic:
 * - no owner -> GRANT;
 * - equal/lower priority never preempts;
 * - PREEMPT_LOWER_PRIORITY requires strictly higher priority and an
 *   interruptible current owner;
 * - QUEUE -> WAIT;
 * - REJECT -> REJECT.
 *
 * Unknown legacy/external audio ownership must not be represented as a known
 * owner here; arbiters continue to treat it as non-preemptible manager busy.
 */
audio_manager_policy_decision_t audio_manager_arbitration_policy_decide(
    const audio_manager_request_t *incoming,
    const audio_manager_policy_owner_t *owner);

const char *audio_manager_policy_decision_to_string(
    audio_manager_policy_decision_t decision);

#ifdef __cplusplus
}
#endif
