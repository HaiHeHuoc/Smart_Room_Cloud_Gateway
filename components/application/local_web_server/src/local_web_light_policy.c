#include "local_web_light_policy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_WEB_LIGHT_NEUTRAL_RED   255U
#define LOCAL_WEB_LIGHT_NEUTRAL_GREEN 255U
#define LOCAL_WEB_LIGHT_NEUTRAL_BLUE  255U

static bool local_web_light_uint8_parse(const char *value,
                                        uint8_t maximum,
                                        uint8_t *parsed)
{
    if ((value == NULL) || (parsed == NULL) || (value[0] < '0') ||
        (value[0] > '9')) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long number = strtoul(value, &end, 10);
    if ((errno == ERANGE) || (end == NULL) || (*end != '\0') ||
        (number > maximum)) {
        return false;
    }
    *parsed = (uint8_t)number;
    return true;
}

static bool local_web_light_effect_parse(const char *value,
                                         light_manager_effect_t *effect)
{
    if ((value == NULL) || (effect == NULL)) {
        return false;
    }
    if (strcmp(value, "solid") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_SOLID;
    } else if (strcmp(value, "blink") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_BLINK;
    } else if (strcmp(value, "breath") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_BREATH;
    } else if (strcmp(value, "pulse") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_PULSE;
    } else if (strcmp(value, "rainbow") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_RAINBOW;
    } else if (strcmp(value, "strobe") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_STROBE;
    } else if (strcmp(value, "heartbeat") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_HEARTBEAT;
    } else if (strcmp(value, "candle") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_CANDLE;
    } else if (strcmp(value, "sos") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_SOS;
    } else if (strcmp(value, "lightning") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_LIGHTNING;
    } else if (strcmp(value, "wake_up") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_WAKE_UP;
    } else if (strcmp(value, "sleep_fade") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_SLEEP_FADE;
    } else if (strcmp(value, "notification") == 0) {
        *effect = LIGHT_MANAGER_EFFECT_NOTIFICATION;
    } else {
        return false;
    }
    return true;
}

static bool local_web_light_state_is_black(const light_manager_state_t *state)
{
    return (state->red == 0U) && (state->green == 0U) &&
           (state->blue == 0U);
}

bool local_web_light_update_set_field(local_web_light_update_t *update,
                                      const char *key,
                                      const char *value)
{
    if ((update == NULL) || (key == NULL) || (value == NULL)) {
        return false;
    }
    if (strcmp(key, "power") == 0) {
        if (update->has_power ||
            ((strcmp(value, "true") != 0) && (strcmp(value, "false") != 0))) {
            return false;
        }
        update->has_power = true;
        update->power_on = strcmp(value, "true") == 0;
        return true;
    }
    if (strcmp(key, "red") == 0) {
        return !update->has_red &&
               local_web_light_uint8_parse(value, UINT8_MAX, &update->red) &&
               (update->has_red = true);
    }
    if (strcmp(key, "green") == 0) {
        return !update->has_green &&
               local_web_light_uint8_parse(value, UINT8_MAX, &update->green) &&
               (update->has_green = true);
    }
    if (strcmp(key, "blue") == 0) {
        return !update->has_blue &&
               local_web_light_uint8_parse(value, UINT8_MAX, &update->blue) &&
               (update->has_blue = true);
    }
    if (strcmp(key, "brightness") == 0) {
        return !update->has_brightness &&
               local_web_light_uint8_parse(value, 100U,
                                           &update->brightness_percent) &&
               (update->has_brightness = true);
    }
    if (strcmp(key, "effect") == 0) {
        return !update->has_effect &&
               local_web_light_effect_parse(value, &update->effect) &&
               (update->has_effect = true);
    }
    return false;
}

bool local_web_light_update_is_valid(const local_web_light_update_t *update)
{
    return (update != NULL) &&
           (update->has_power || update->has_red || update->has_green ||
            update->has_blue || update->has_brightness || update->has_effect) &&
           !(update->has_effect && update->has_power && !update->power_on);
}

bool local_web_light_apply_update(const local_web_light_update_t *update,
                                  light_manager_state_t *state)
{
    if (!local_web_light_update_is_valid(update) || (state == NULL)) {
        return false;
    }
    if (update->has_power) state->power_on = update->power_on;
    if (update->has_red) state->red = update->red;
    if (update->has_green) state->green = update->green;
    if (update->has_blue) state->blue = update->blue;
    if (update->has_brightness) state->brightness_percent = update->brightness_percent;
    if (update->has_effect) {
        state->effect = update->effect;
        /* Match MCP: an effect means visible activation unless a color was
         * explicitly supplied. A black boot state gets neutral white. */
        if (!update->has_power) state->power_on = true;
        if (!update->has_red && !update->has_green && !update->has_blue &&
            local_web_light_state_is_black(state)) {
            state->red = LOCAL_WEB_LIGHT_NEUTRAL_RED;
            state->green = LOCAL_WEB_LIGHT_NEUTRAL_GREEN;
            state->blue = LOCAL_WEB_LIGHT_NEUTRAL_BLUE;
        }
    }
    return true;
}

const char *local_web_light_effect_name(light_manager_effect_t effect)
{
    switch (effect) {
    case LIGHT_MANAGER_EFFECT_SOLID: return "solid";
    case LIGHT_MANAGER_EFFECT_BLINK: return "blink";
    case LIGHT_MANAGER_EFFECT_BREATH: return "breath";
    case LIGHT_MANAGER_EFFECT_PULSE: return "pulse";
    case LIGHT_MANAGER_EFFECT_RAINBOW: return "rainbow";
    case LIGHT_MANAGER_EFFECT_STROBE: return "strobe";
    case LIGHT_MANAGER_EFFECT_HEARTBEAT: return "heartbeat";
    case LIGHT_MANAGER_EFFECT_CANDLE: return "candle";
    case LIGHT_MANAGER_EFFECT_SOS: return "sos";
    case LIGHT_MANAGER_EFFECT_LIGHTNING: return "lightning";
    case LIGHT_MANAGER_EFFECT_WAKE_UP: return "wake_up";
    case LIGHT_MANAGER_EFFECT_SLEEP_FADE: return "sleep_fade";
    case LIGHT_MANAGER_EFFECT_NOTIFICATION: return "notification";
    default: return NULL;
    }
}

local_web_light_manager_result_t local_web_light_manager_result_from_error(
    esp_err_t error)
{
    if (error == ESP_OK) return LOCAL_WEB_LIGHT_MANAGER_RESULT_OK;
    if (error == ESP_ERR_INVALID_STATE) return LOCAL_WEB_LIGHT_MANAGER_RESULT_UNAVAILABLE;
    if (error == ESP_ERR_TIMEOUT) return LOCAL_WEB_LIGHT_MANAGER_RESULT_BUSY;
    return LOCAL_WEB_LIGHT_MANAGER_RESULT_FAILED;
}
