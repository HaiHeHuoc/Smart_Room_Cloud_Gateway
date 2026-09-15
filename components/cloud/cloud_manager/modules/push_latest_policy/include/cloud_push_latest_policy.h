#pragma once

#include <stdbool.h>

#include "cloud_manager.h"

/**
 * @brief Copied local facts used to classify a bounded push-latest request.
 *
 * This private policy has no Firebase, HTTP, task, or queue dependencies so
 * its deterministic admission rules can be host-tested independently.
 */
typedef struct
{
    bool initialized;
    bool started;
    bool has_ipv4_address;
    bool has_latest_telemetry;
    bool request_pending;
    cloud_manager_state_t state;
} cloud_push_latest_policy_input_t;

/** Classify whether one latest-value upload request may be retained. */
cloud_manager_push_latest_outcome_t cloud_push_latest_policy_evaluate(
    const cloud_push_latest_policy_input_t *input);
