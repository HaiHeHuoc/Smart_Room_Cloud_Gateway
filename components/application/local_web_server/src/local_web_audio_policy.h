#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOCAL_WEB_AUDIO_ACTION_PAUSE = 0,
    LOCAL_WEB_AUDIO_ACTION_RESUME,
    LOCAL_WEB_AUDIO_ACTION_RESTART,
    LOCAL_WEB_AUDIO_ACTION_STOP,
} local_web_audio_action_t;

/** Parse the exact bounded HTTP action tokens accepted by the Web API. */
bool local_web_audio_action_parse(const char *value, local_web_audio_action_t *action);

/** Parse an unsigned decimal playback volume in the inclusive range 0..100. */
bool local_web_audio_volume_percent_parse(const char *value, uint32_t *percent);

/** Parse one bounded unsigned decimal frame position or generation. */
bool local_web_audio_uint64_parse(const char *value, uint64_t *parsed);
