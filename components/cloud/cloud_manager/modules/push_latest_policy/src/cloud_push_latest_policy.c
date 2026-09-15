#include "cloud_push_latest_policy.h"

cloud_manager_push_latest_outcome_t cloud_push_latest_policy_evaluate(
    const cloud_push_latest_policy_input_t *input)
{
    if ((input == NULL) || !input->initialized || !input->started ||
        (input->state == CLOUD_MANAGER_STATE_UNINITIALIZED) ||
        (input->state == CLOUD_MANAGER_STATE_INITIALIZED)) {
        return CLOUD_MANAGER_PUSH_LATEST_NOT_READY;
    }

    if ((input->state == CLOUD_MANAGER_STATE_AUTH_ERROR) ||
        (input->state == CLOUD_MANAGER_STATE_ERROR)) {
        return CLOUD_MANAGER_PUSH_LATEST_INVALID_STATE;
    }

    if (!input->has_ipv4_address) {
        return CLOUD_MANAGER_PUSH_LATEST_OFFLINE;
    }

    if (!input->has_latest_telemetry) {
        return CLOUD_MANAGER_PUSH_LATEST_NOT_READY;
    }

    if (input->request_pending ||
        (input->state == CLOUD_MANAGER_STATE_UPLOADING) ||
        (input->state == CLOUD_MANAGER_STATE_RETRY_WAIT)) {
        return CLOUD_MANAGER_PUSH_LATEST_BUSY;
    }

    return CLOUD_MANAGER_PUSH_LATEST_ACCEPTED;
}
