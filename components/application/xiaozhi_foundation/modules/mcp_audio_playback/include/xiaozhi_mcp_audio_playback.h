#pragma once

#include "esp_mcp_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Attach audio.control_playback and audio.get_playback_state. */
esp_err_t xiaozhi_mcp_audio_playback_attach(esp_mcp_t *mcp);

/** Reset attachment state before the owning MCP engine is destroyed. */
void xiaozhi_mcp_audio_playback_detach(void);

#ifdef __cplusplus
}
#endif
