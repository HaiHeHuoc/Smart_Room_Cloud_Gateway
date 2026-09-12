#include "smart_room_mcp_adapter_internal.h"

#include <limits.h>
#include <stdio.h>

#include "cloud_manager.h"
#include "esp_timer.h"
#include "xiaozhi_foundation.h"

static const char *app_xiaozhi_cloud_state_to_string(
    cloud_manager_state_t state)
{
    switch (state) {
        case CLOUD_MANAGER_STATE_INITIALIZED:
            return "initialized";
        case CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK:
            return "waiting_for_network";
        case CLOUD_MANAGER_STATE_WAITING_FOR_DATA:
            return "waiting_for_data";
        case CLOUD_MANAGER_STATE_UPLOADING:
            return "uploading";
        case CLOUD_MANAGER_STATE_ONLINE:
            return "online";
        case CLOUD_MANAGER_STATE_RETRY_WAIT:
            return "retry_wait";
        case CLOUD_MANAGER_STATE_AUTH_ERROR:
            return "auth_error";
        case CLOUD_MANAGER_STATE_ERROR:
            return "error";
        case CLOUD_MANAGER_STATE_UNINITIALIZED:
        default:
            return "uninitialized";
    }
}

static const char *app_xiaozhi_cloud_failure_class_to_string(
    cloud_manager_failure_class_t failure_class)
{
    switch (failure_class) {
        case CLOUD_MANAGER_FAILURE_NONE:
            return "none";
        case CLOUD_MANAGER_FAILURE_WAIT_FOR_NETWORK:
            return "waiting_for_network";
        case CLOUD_MANAGER_FAILURE_RETRYABLE_TRANSPORT:
            return "retryable_transport";
        case CLOUD_MANAGER_FAILURE_RETRYABLE_HTTP:
            return "retryable_http";
        case CLOUD_MANAGER_FAILURE_REAUTH_REQUIRED:
            return "reauth_required";
        case CLOUD_MANAGER_FAILURE_AUTH_FATAL:
            return "auth_fatal";
        case CLOUD_MANAGER_FAILURE_NONRETRYABLE_CONFIG:
            return "nonretryable_config";
        case CLOUD_MANAGER_FAILURE_NONRETRYABLE_INTERNAL:
            return "nonretryable_internal";
        default:
            return "unknown";
    }
}

static uint32_t app_xiaozhi_cloud_milliseconds_to_seconds_ceil(
    uint32_t milliseconds)
{
    return (milliseconds == 0U) ? 0U :
        ((milliseconds - 1U) / 1000U) + 1U;
}

static esp_err_t app_xiaozhi_copy_cloud_sync_status(
    xiaozhi_foundation_cloud_sync_query_snapshot_t *snapshot,
    void *user_context)
{
    (void)user_context;
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = (xiaozhi_foundation_cloud_sync_query_snapshot_t){0};
    cloud_manager_status_t cloud = {0};
    const esp_err_t ret = cloud_manager_get_status(&cloud);
    if (ret != ESP_OK) {
        return ret;
    }

    (void)snprintf(snapshot->state, sizeof(snapshot->state), "%s",
                   app_xiaozhi_cloud_state_to_string(cloud.state));
    (void)snprintf(snapshot->failure_class, sizeof(snapshot->failure_class),
                   "%s", app_xiaozhi_cloud_failure_class_to_string(
                       cloud.last_failure_class));
    snapshot->available = true;
    snapshot->consecutive_failure_count = cloud.consecutive_failure_count;
    snapshot->retry_scheduled = (cloud.state == CLOUD_MANAGER_STATE_RETRY_WAIT);
    if (snapshot->retry_scheduled) {
        snapshot->retry_delay_seconds =
            app_xiaozhi_cloud_milliseconds_to_seconds_ceil(
                cloud.current_retry_delay_ms);
    }

    const int64_t now_ms = esp_timer_get_time() / 1000;
    if ((cloud.last_success_time_ms > 0) && (now_ms >= cloud.last_success_time_ms)) {
        const int64_t age_ms = now_ms - cloud.last_success_time_ms;
        const int64_t age_seconds = age_ms / 1000;
        snapshot->last_success_available = true;
        snapshot->last_success_age_seconds = (age_seconds > (int64_t)UINT32_MAX)
            ? UINT32_MAX : (uint32_t)age_seconds;
    }

    return ESP_OK;
}

esp_err_t smart_room_mcp_cloud_sync_register_provider(void)
{
    return xiaozhi_foundation_register_cloud_sync_query_provider(
        app_xiaozhi_copy_cloud_sync_status,
        NULL);
}
