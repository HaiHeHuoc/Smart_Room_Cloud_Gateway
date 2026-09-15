#pragma once

#include <stdbool.h>

#include "xiaozhi_foundation.h"

/** Parse the exact bounded audio.control_playback action allowlist. */
bool xiaozhi_mcp_audio_playback_parse_action(
    const char *value,
    xiaozhi_foundation_audio_action_t *action);
