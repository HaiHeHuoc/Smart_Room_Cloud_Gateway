#pragma once

#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attach the controlled cloud.push_latest tool to a fresh MCP engine. */
esp_err_t xiaozhi_mcp_cloud_push_latest_attach(esp_mcp_t *mcp);

/** Reset attachment state before the owning MCP engine is destroyed. */
void xiaozhi_mcp_cloud_push_latest_detach(void);

#ifdef __cplusplus
}
#endif
