#pragma once

#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attach bounded audio playback state/control and catalog tools. */
esp_err_t xiaozhi_mcp_audio_playback_attach(esp_mcp_t *mcp);

/** Reset attachment state before the owning MCP engine is destroyed. */
void xiaozhi_mcp_audio_playback_detach(void);

#ifdef __cplusplus
}
#endif
