#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "light_manager.h"

typedef struct
{
    bool has_power;
    bool power_on;
    bool has_red;
    uint8_t red;
    bool has_green;
    uint8_t green;
    bool has_blue;
    uint8_t blue;
    bool has_brightness;
    uint8_t brightness_percent;
    bool has_effect;
    light_manager_effect_t effect;
} local_web_light_update_t;

typedef enum
{
    LOCAL_WEB_LIGHT_MANAGER_RESULT_OK = 0,
    LOCAL_WEB_LIGHT_MANAGER_RESULT_UNAVAILABLE,
    LOCAL_WEB_LIGHT_MANAGER_RESULT_BUSY,
    LOCAL_WEB_LIGHT_MANAGER_RESULT_FAILED,
} local_web_light_manager_result_t;

/** Parse one exact query key/value pair and reject unknown or duplicate fields. */
bool local_web_light_update_set_field(local_web_light_update_t *update,
                                      const char *key,
                                      const char *value);

/** Require at least one field and reject an invisible effect activation request. */
bool local_web_light_update_is_valid(const local_web_light_update_t *update);

/** Apply a validated partial update to a copied manager state. */
bool local_web_light_apply_update(const local_web_light_update_t *update,
                                  light_manager_state_t *state);

/** Map the fixed product effect vocabulary to its Web token. */
const char *local_web_light_effect_name(light_manager_effect_t effect);

/** Normalize manager failures into bounded Web-facing categories. */
local_web_light_manager_result_t local_web_light_manager_result_from_error(
    esp_err_t error);
