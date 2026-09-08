#include "audio_manager_arbitration.h"

#include <stddef.h>

static bool audio_manager_client_is_valid(audio_manager_client_t client)
{
    return (client > AUDIO_MANAGER_CLIENT_NONE) &&
           (client < AUDIO_MANAGER_CLIENT_COUNT);
}

static bool audio_manager_resource_is_valid(audio_manager_resource_t resource)
{
    return (resource == AUDIO_MANAGER_RESOURCE_CAPTURE) ||
           (resource == AUDIO_MANAGER_RESOURCE_PLAYBACK);
}

static bool audio_manager_busy_policy_is_valid(audio_manager_busy_policy_t policy)
{
    return (policy == AUDIO_MANAGER_BUSY_REJECT) ||
           (policy == AUDIO_MANAGER_BUSY_QUEUE) ||
           (policy == AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY);
}

static uint8_t audio_manager_default_priority(audio_manager_client_t client)
{
    switch (client) {
        case AUDIO_MANAGER_CLIENT_ALARM:
            return AUDIO_MANAGER_PRIORITY_CRITICAL_ALARM;
        case AUDIO_MANAGER_CLIENT_SYSTEM:
            return AUDIO_MANAGER_PRIORITY_SYSTEM;
        case AUDIO_MANAGER_CLIENT_XIAOZHI:
            return AUDIO_MANAGER_PRIORITY_XIAOZHI;
        case AUDIO_MANAGER_CLIENT_NOTIFICATION:
            return AUDIO_MANAGER_PRIORITY_NOTIFICATION;
        case AUDIO_MANAGER_CLIENT_UI:
            return AUDIO_MANAGER_PRIORITY_UI;
        case AUDIO_MANAGER_CLIENT_RECORDER:
        case AUDIO_MANAGER_CLIENT_TEST:
            return AUDIO_MANAGER_PRIORITY_BACKGROUND;
        case AUDIO_MANAGER_CLIENT_NONE:
        case AUDIO_MANAGER_CLIENT_COUNT:
        default:
            return AUDIO_MANAGER_PRIORITY_MIN;
    }
}

static audio_manager_busy_policy_t audio_manager_default_busy_policy(
    audio_manager_client_t client)
{
    switch (client) {
        case AUDIO_MANAGER_CLIENT_ALARM:
            return AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY;
        case AUDIO_MANAGER_CLIENT_NOTIFICATION:
        case AUDIO_MANAGER_CLIENT_UI:
            return AUDIO_MANAGER_BUSY_QUEUE;
        case AUDIO_MANAGER_CLIENT_SYSTEM:
        case AUDIO_MANAGER_CLIENT_XIAOZHI:
        case AUDIO_MANAGER_CLIENT_RECORDER:
        case AUDIO_MANAGER_CLIENT_TEST:
        case AUDIO_MANAGER_CLIENT_NONE:
        case AUDIO_MANAGER_CLIENT_COUNT:
        default:
            return AUDIO_MANAGER_BUSY_REJECT;
    }
}

static bool audio_manager_default_interruptible(audio_manager_client_t client)
{
    switch (client) {
        case AUDIO_MANAGER_CLIENT_ALARM:
        case AUDIO_MANAGER_CLIENT_SYSTEM:
            return false;
        case AUDIO_MANAGER_CLIENT_XIAOZHI:
        case AUDIO_MANAGER_CLIENT_NOTIFICATION:
        case AUDIO_MANAGER_CLIENT_RECORDER:
        case AUDIO_MANAGER_CLIENT_UI:
        case AUDIO_MANAGER_CLIENT_TEST:
        case AUDIO_MANAGER_CLIENT_NONE:
        case AUDIO_MANAGER_CLIENT_COUNT:
        default:
            return true;
    }
}

esp_err_t audio_manager_request_make_default(
    uint32_t request_id,
    audio_manager_client_t client,
    audio_manager_resource_t resource,
    audio_manager_request_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((request_id == 0U) ||
        !audio_manager_client_is_valid(client) ||
        !audio_manager_resource_is_valid(resource)) {
        return ESP_ERR_INVALID_ARG;
    }

    const audio_manager_request_t result = {
        .request_id = request_id,
        .client = client,
        .resource = resource,
        .priority = audio_manager_default_priority(client),
        .busy_policy = audio_manager_default_busy_policy(client),
        .interruptible = audio_manager_default_interruptible(client),
    };

    *request = result;
    return ESP_OK;
}

esp_err_t audio_manager_request_validate(const audio_manager_request_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((request->request_id == 0U) ||
        !audio_manager_client_is_valid(request->client) ||
        !audio_manager_resource_is_valid(request->resource) ||
        !audio_manager_busy_policy_is_valid(request->busy_policy)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

const char *audio_manager_client_to_string(audio_manager_client_t client)
{
    switch (client) {
        case AUDIO_MANAGER_CLIENT_NONE: return "NONE";
        case AUDIO_MANAGER_CLIENT_SYSTEM: return "SYSTEM";
        case AUDIO_MANAGER_CLIENT_XIAOZHI: return "XIAOZHI";
        case AUDIO_MANAGER_CLIENT_NOTIFICATION: return "NOTIFICATION";
        case AUDIO_MANAGER_CLIENT_ALARM: return "ALARM";
        case AUDIO_MANAGER_CLIENT_RECORDER: return "RECORDER";
        case AUDIO_MANAGER_CLIENT_UI: return "UI";
        case AUDIO_MANAGER_CLIENT_TEST: return "TEST";
        case AUDIO_MANAGER_CLIENT_COUNT:
        default: return "UNKNOWN";
    }
}

const char *audio_manager_resource_to_string(audio_manager_resource_t resource)
{
    switch (resource) {
        case AUDIO_MANAGER_RESOURCE_NONE: return "NONE";
        case AUDIO_MANAGER_RESOURCE_CAPTURE: return "CAPTURE";
        case AUDIO_MANAGER_RESOURCE_PLAYBACK: return "PLAYBACK";
        default: return "UNKNOWN";
    }
}

const char *audio_manager_busy_policy_to_string(audio_manager_busy_policy_t policy)
{
    switch (policy) {
        case AUDIO_MANAGER_BUSY_REJECT: return "REJECT";
        case AUDIO_MANAGER_BUSY_QUEUE: return "QUEUE";
        case AUDIO_MANAGER_BUSY_PREEMPT_LOWER_PRIORITY: return "PREEMPT_LOWER_PRIORITY";
        default: return "UNKNOWN";
    }
}
