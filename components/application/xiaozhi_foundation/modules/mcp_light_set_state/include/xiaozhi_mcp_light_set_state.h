#pragma once

#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attach the controlled Smart Room light.set_state tool to a fresh MCP engine. */
esp_err_t xiaozhi_mcp_light_set_state_attach(esp_mcp_t *mcp);

/** Reset attachment state after the owning production chat session stops. */
void xiaozhi_mcp_light_set_state_detach(void);

#ifdef __cplusplus
}
#endif
