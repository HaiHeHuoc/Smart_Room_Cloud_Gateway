#include <stdbool.h>
#include <stdio.h>

#include "cloud_push_latest_policy.h"

static bool expect_outcome(
    cloud_push_latest_policy_input_t input,
    cloud_manager_push_latest_outcome_t expected)
{
    return cloud_push_latest_policy_evaluate(&input) == expected;
}

int main(void)
{
    const cloud_push_latest_policy_input_t ready = {
        .initialized = true,
        .started = true,
        .has_ipv4_address = true,
        .has_latest_telemetry = true,
        .request_pending = false,
        .state = CLOUD_MANAGER_STATE_ONLINE,
    };

    bool ok = true;
    ok &= expect_outcome(ready, CLOUD_MANAGER_PUSH_LATEST_ACCEPTED);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = false,
            .started = false,
            .state = CLOUD_MANAGER_STATE_UNINITIALIZED,
        },
        CLOUD_MANAGER_PUSH_LATEST_NOT_READY);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = true,
            .started = true,
            .has_latest_telemetry = true,
            .state = CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK,
        },
        CLOUD_MANAGER_PUSH_LATEST_OFFLINE);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = true,
            .started = true,
            .has_ipv4_address = true,
            .has_latest_telemetry = true,
            .request_pending = true,
            .state = CLOUD_MANAGER_STATE_ONLINE,
        },
        CLOUD_MANAGER_PUSH_LATEST_BUSY);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = true,
            .started = true,
            .has_ipv4_address = true,
            .has_latest_telemetry = true,
            .state = CLOUD_MANAGER_STATE_UPLOADING,
        },
        CLOUD_MANAGER_PUSH_LATEST_BUSY);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = true,
            .started = true,
            .has_ipv4_address = true,
            .has_latest_telemetry = true,
            .state = CLOUD_MANAGER_STATE_RETRY_WAIT,
        },
        CLOUD_MANAGER_PUSH_LATEST_BUSY);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = true,
            .started = true,
            .has_ipv4_address = true,
            .has_latest_telemetry = true,
            .state = CLOUD_MANAGER_STATE_AUTH_ERROR,
        },
        CLOUD_MANAGER_PUSH_LATEST_INVALID_STATE);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = true,
            .started = true,
            .has_ipv4_address = true,
            .has_latest_telemetry = true,
            .state = CLOUD_MANAGER_STATE_ERROR,
        },
        CLOUD_MANAGER_PUSH_LATEST_INVALID_STATE);
    ok &= expect_outcome(
        (cloud_push_latest_policy_input_t){
            .initialized = true,
            .started = true,
            .has_ipv4_address = true,
            .has_latest_telemetry = false,
            .state = CLOUD_MANAGER_STATE_WAITING_FOR_DATA,
        },
        CLOUD_MANAGER_PUSH_LATEST_NOT_READY);
    ok &= cloud_push_latest_policy_evaluate(NULL) ==
        CLOUD_MANAGER_PUSH_LATEST_NOT_READY;

    puts(ok ? "Cloud push-latest admission policy: PASS" :
              "Cloud push-latest admission policy: FAIL");
    return ok ? 0 : 1;
}
