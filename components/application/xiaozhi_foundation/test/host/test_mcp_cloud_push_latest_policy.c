#include <stdbool.h>
#include <stdio.h>

#include "xiaozhi_mcp_cloud_push_latest_policy.h"

int main(void)
{
    bool ok = true;
    ok &= xiaozhi_mcp_cloud_push_latest_is_accepted(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_ACCEPTED);
    ok &= !xiaozhi_mcp_cloud_push_latest_is_accepted(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_BUSY);
    ok &= xiaozhi_mcp_cloud_push_latest_outcome_name(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_ACCEPTED)[0] == 'a';
    ok &= xiaozhi_mcp_cloud_push_latest_outcome_name(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_NOT_READY)[0] == 'n';
    ok &= xiaozhi_mcp_cloud_push_latest_outcome_name(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_OFFLINE)[0] == 'o';
    ok &= xiaozhi_mcp_cloud_push_latest_outcome_name(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_BUSY)[0] == 'b';
    ok &= xiaozhi_mcp_cloud_push_latest_outcome_name(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_INVALID_STATE)[0] == 'i';
    ok &= xiaozhi_mcp_cloud_push_latest_outcome_name(
        XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_FAILED)[0] == 'f';

    puts(ok ? "MCP cloud push-latest result policy: PASS" :
              "MCP cloud push-latest result policy: FAIL");
    return ok ? 0 : 1;
}
