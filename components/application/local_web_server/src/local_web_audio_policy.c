#include "local_web_audio_policy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

bool local_web_audio_action_parse(const char *value, local_web_audio_action_t *action)
{
    if ((value == NULL) || (action == NULL)) {
        return false;
    }
    if (strcmp(value, "pause") == 0) {
        *action = LOCAL_WEB_AUDIO_ACTION_PAUSE;
    } else if (strcmp(value, "resume") == 0) {
        *action = LOCAL_WEB_AUDIO_ACTION_RESUME;
    } else if (strcmp(value, "restart") == 0) {
        *action = LOCAL_WEB_AUDIO_ACTION_RESTART;
    } else if (strcmp(value, "stop") == 0) {
        *action = LOCAL_WEB_AUDIO_ACTION_STOP;
    } else {
        return false;
    }
    return true;
}

bool local_web_audio_volume_percent_parse(const char *value, uint32_t *percent)
{
    if ((value == NULL) || (percent == NULL) || (value[0] == '\0')) {
        return false;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);
    if ((end == NULL) || (*end != '\0') || (parsed > 100U)) {
        return false;
    }
    *percent = (uint32_t)parsed;
    return true;
}

bool local_web_audio_uint64_parse(const char *value, uint64_t *parsed)
{
    if ((value == NULL) || (parsed == NULL) || (value[0] < '0') ||
        (value[0] > '9')) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value_parsed = strtoull(value, &end, 10);
    if ((errno == ERANGE) || (end == NULL) || (*end != '\0')) {
        return false;
    }
    *parsed = (uint64_t)value_parsed;
    return true;
}
