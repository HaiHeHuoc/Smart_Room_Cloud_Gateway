#pragma once

#include <stdbool.h>

#include "xiaozhi_foundation.h"

/** Return the fixed MCP-safe token for a bounded cloud-push outcome. */
const char *xiaozhi_mcp_cloud_push_latest_outcome_name(
    xiaozhi_foundation_cloud_push_latest_outcome_t outcome);

/** True only for scheduling acceptance, never for an upload result. */
bool xiaozhi_mcp_cloud_push_latest_is_accepted(
    xiaozhi_foundation_cloud_push_latest_outcome_t outcome);
