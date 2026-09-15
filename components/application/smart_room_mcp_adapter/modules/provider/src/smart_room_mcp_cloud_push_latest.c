#include "smart_room_mcp_adapter_internal.h"

#include "cloud_manager.h"
#include "xiaozhi_foundation.h"

static xiaozhi_foundation_cloud_push_latest_outcome_t
app_xiaozhi_cloud_push_latest_outcome_from_manager(
    cloud_manager_push_latest_outcome_t outcome)
{
    switch (outcome) {
    case CLOUD_MANAGER_PUSH_LATEST_ACCEPTED:
        return XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_ACCEPTED;
    case CLOUD_MANAGER_PUSH_LATEST_NOT_READY:
        return XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_NOT_READY;
    case CLOUD_MANAGER_PUSH_LATEST_OFFLINE:
        return XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_OFFLINE;
    case CLOUD_MANAGER_PUSH_LATEST_BUSY:
        return XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_BUSY;
    case CLOUD_MANAGER_PUSH_LATEST_INVALID_STATE:
        return XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_INVALID_STATE;
    case CLOUD_MANAGER_PUSH_LATEST_FAILED:
    default:
        return XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_FAILED;
    }
}

static esp_err_t app_xiaozhi_request_cloud_push_latest(
    xiaozhi_foundation_cloud_push_latest_result_t *result,
    void *user_context)
{
    (void)user_context;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *result = (xiaozhi_foundation_cloud_push_latest_result_t){
        .outcome = XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_FAILED,
        .accepted = false,
    };

    cloud_manager_push_latest_result_t manager_result = {0};
    const esp_err_t ret = cloud_manager_request_push_latest(&manager_result);
    if (ret != ESP_OK) {
        return ret;
    }

    result->outcome = app_xiaozhi_cloud_push_latest_outcome_from_manager(
        manager_result.outcome);
    result->accepted = manager_result.accepted &&
        (result->outcome == XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_ACCEPTED);
    return ESP_OK;
}

esp_err_t smart_room_mcp_cloud_push_latest_register_provider(void)
{
    return xiaozhi_foundation_register_cloud_push_latest_provider(
        app_xiaozhi_request_cloud_push_latest,
        NULL);
}
