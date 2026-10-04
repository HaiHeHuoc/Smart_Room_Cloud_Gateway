#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xiaozhi_foundation.h"

/** Borrowed fields returned by esp_xiaozhi_chat_get_info(). */
typedef struct {
    bool has_activation_code;
    const char *activation_code;
    const char *activation_message;
    int activation_timeout_ms;
} xiaozhi_activation_upstream_info_t;

/** Clear all project-owned activation facts before a new backend response. */
void xiaozhi_activation_status_clear(
    xiaozhi_foundation_activation_status_t *status);

/**
 * Convert one upstream response into a bounded project-owned snapshot.
 *
 * Returns true only when the feature is enabled and the backend explicitly
 * supplied an activation code. Neither input pointer is retained.
 */
bool xiaozhi_activation_status_from_upstream(
    bool feature_enabled,
    const xiaozhi_activation_upstream_info_t *upstream,
    xiaozhi_foundation_activation_status_t *status);

/** Return the project-owned bounded activation retry delay for an attempt. */
uint32_t xiaozhi_foundation_activation_retry_delay_ms(uint32_t attempt);
