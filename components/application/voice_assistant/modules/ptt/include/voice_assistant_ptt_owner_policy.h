#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "voice_assistant_ptt.h"

/*
 * Small deterministic ownership/lease state used by the PTT policy task.
 * The parent PTT mutex protects every access; this helper owns no task,
 * queue, timer, audio resource, or transport handle.
 */
typedef struct {
    voice_assistant_ptt_source_t source;
    uint32_t client_id;
    uint32_t ptt_generation;
    bool pressed;
    int64_t web_lease_expires_at_us;
    bool web_lease_expiring;
} voice_assistant_ptt_owner_policy_t;

bool voice_assistant_ptt_owner_policy_reserve(
    voice_assistant_ptt_owner_policy_t *policy,
    voice_assistant_ptt_source_t source,
    uint32_t client_id,
    uint32_t ptt_generation,
    int64_t now_us);

bool voice_assistant_ptt_owner_policy_matches(
    const voice_assistant_ptt_owner_policy_t *policy,
    voice_assistant_ptt_source_t source,
    uint32_t client_id,
    uint32_t ptt_generation);

bool voice_assistant_ptt_owner_policy_refresh_web_lease(
    voice_assistant_ptt_owner_policy_t *policy,
    uint32_t client_id,
    uint32_t ptt_generation,
    int64_t now_us);

bool voice_assistant_ptt_owner_policy_web_lease_expired(
    voice_assistant_ptt_owner_policy_t *policy,
    int64_t now_us,
    uint32_t *client_id,
    uint32_t *ptt_generation);

void voice_assistant_ptt_owner_policy_release(
    voice_assistant_ptt_owner_policy_t *policy);
