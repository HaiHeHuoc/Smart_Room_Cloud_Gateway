#include "xiaozhi_mcp_cloud_push_latest_policy.h"

const char *xiaozhi_mcp_cloud_push_latest_outcome_name(
    xiaozhi_foundation_cloud_push_latest_outcome_t outcome)
{
    switch (outcome) {
    case XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_ACCEPTED:
        return "accepted";
    case XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_NOT_READY:
        return "not_ready";
    case XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_OFFLINE:
        return "offline";
    case XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_BUSY:
        return "busy";
    case XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_INVALID_STATE:
        return "invalid_state";
    case XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_FAILED:
    default:
        return "failed";
    }
}

bool xiaozhi_mcp_cloud_push_latest_is_accepted(
    xiaozhi_foundation_cloud_push_latest_outcome_t outcome)
{
    return outcome == XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_ACCEPTED;
}
