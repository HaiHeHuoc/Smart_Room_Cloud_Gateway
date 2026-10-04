#include "xiaozhi_activation_policy.h"

#include <string.h>

#define XIAOZHI_ACTIVATION_RETRY_INITIAL_MS 10000U
#define XIAOZHI_ACTIVATION_RETRY_MAX_MS     60000U

static void xiaozhi_activation_copy_text(
    char *destination,
    size_t destination_size,
    bool *available,
    bool *truncated,
    const char *source)
{
    if ((destination == NULL) || (destination_size == 0U) ||
        (available == NULL) || (truncated == NULL)) {
        return;
    }

    *available = false;
    *truncated = false;
    destination[0] = '\0';
    if (source == NULL) {
        return;
    }

    const size_t length = strnlen(source, destination_size - 1U);
    memcpy(destination, source, length);
    destination[length] = '\0';
    *available = true;
    *truncated = (source[length] != '\0');
}

void xiaozhi_activation_status_clear(
    xiaozhi_foundation_activation_status_t *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

bool xiaozhi_activation_status_from_upstream(
    bool feature_enabled,
    const xiaozhi_activation_upstream_info_t *upstream,
    xiaozhi_foundation_activation_status_t *status)
{
    if (status == NULL) {
        return false;
    }

    xiaozhi_activation_status_clear(status);
    if (!feature_enabled || (upstream == NULL) ||
        !upstream->has_activation_code ||
        (upstream->activation_code == NULL) ||
        (upstream->activation_code[0] == '\0')) {
        return false;
    }

    status->required = true;
    xiaozhi_activation_copy_text(
        status->code,
        sizeof(status->code),
        &status->code_available,
        &status->code_truncated,
        upstream->activation_code);
    xiaozhi_activation_copy_text(
        status->message,
        sizeof(status->message),
        &status->message_available,
        &status->message_truncated,
        upstream->activation_message);
    if (upstream->activation_timeout_ms > 0) {
        status->backend_timeout_ms = (uint32_t)upstream->activation_timeout_ms;
    }
    return status->code_available;
}

uint32_t xiaozhi_foundation_activation_retry_delay_ms(uint32_t attempt)
{
    uint32_t delay_ms = XIAOZHI_ACTIVATION_RETRY_INITIAL_MS;
    while ((attempt > 1U) && (delay_ms < XIAOZHI_ACTIVATION_RETRY_MAX_MS)) {
        const uint32_t remaining = XIAOZHI_ACTIVATION_RETRY_MAX_MS - delay_ms;
        delay_ms += (delay_ms > remaining) ? remaining : delay_ms;
        --attempt;
    }
    return delay_ms;
}
