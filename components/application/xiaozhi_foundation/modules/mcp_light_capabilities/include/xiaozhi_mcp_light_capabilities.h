#pragma once

#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attach the static, read-only light capability tool to a fresh MCP engine. */
esp_err_t xiaozhi_mcp_light_capabilities_attach(esp_mcp_t *mcp);

/** Reset attachment state after the owning chat session has stopped. */
void xiaozhi_mcp_light_capabilities_detach(void);

#ifdef __cplusplus
}
#endif
