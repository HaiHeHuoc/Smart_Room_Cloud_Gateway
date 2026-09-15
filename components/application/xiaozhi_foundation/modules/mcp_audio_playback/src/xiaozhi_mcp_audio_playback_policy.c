#include "xiaozhi_mcp_audio_playback_policy.h"

#include <string.h>

#define XIAOZHI_AUDIO_ACTION_MAX_BYTES 7U

bool xiaozhi_mcp_audio_playback_parse_action(
    const char *value,
    xiaozhi_foundation_audio_action_t *action)
{
    if ((value == NULL) || (action == NULL) ||
        (strlen(value) > XIAOZHI_AUDIO_ACTION_MAX_BYTES)) {
        return false;
    }
    if (strcmp(value, "pause") == 0) {
        *action = XIAOZHI_FOUNDATION_AUDIO_ACTION_PAUSE;
    } else if (strcmp(value, "resume") == 0) {
        *action = XIAOZHI_FOUNDATION_AUDIO_ACTION_RESUME;
    } else if (strcmp(value, "stop") == 0) {
        *action = XIAOZHI_FOUNDATION_AUDIO_ACTION_STOP;
    } else if (strcmp(value, "restart") == 0) {
        *action = XIAOZHI_FOUNDATION_AUDIO_ACTION_RESTART;
    } else {
        return false;
    }
    return true;
}
