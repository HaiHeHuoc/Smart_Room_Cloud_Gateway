#include "xiaozhi_light_set_state_composition.h"

#include "light_manager.h"
#include "xiaozhi_foundation.h"

#define APP_XIAOZHI_LIGHT_EFFECT_DEFAULT_RED   255U
#define APP_XIAOZHI_LIGHT_EFFECT_DEFAULT_GREEN 255U
#define APP_XIAOZHI_LIGHT_EFFECT_DEFAULT_BLUE  255U

static bool app_xiaozhi_light_color_is_black(const light_manager_state_t *state)
{
    return (state->red == 0U) && (state->green == 0U) && (state->blue == 0U);
}

static xiaozhi_foundation_light_set_state_outcome_t
app_xiaozhi_light_outcome_from_error(
    esp_err_t error,
    xiaozhi_foundation_light_set_state_outcome_t fallback)
{
    return (error == ESP_ERR_INVALID_STATE)
        ? XIAOZHI_FOUNDATION_LIGHT_SET_STATE_MANAGER_NOT_INITIALIZED
        : fallback;
}

static bool app_xiaozhi_light_effect_to_manager(
    xiaozhi_foundation_light_effect_t effect,
    light_manager_effect_t *manager_effect)
{
    if (manager_effect == NULL) {
        return false;
    }

    switch (effect) {
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_SOLID:
        *manager_effect = LIGHT_MANAGER_EFFECT_SOLID;
        return true;
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_BLINK:
        *manager_effect = LIGHT_MANAGER_EFFECT_BLINK;
        return true;
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_BREATH:
        *manager_effect = LIGHT_MANAGER_EFFECT_BREATH;
        return true;
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_PULSE:
        *manager_effect = LIGHT_MANAGER_EFFECT_PULSE;
        return true;
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_RAINBOW:
        *manager_effect = LIGHT_MANAGER_EFFECT_RAINBOW;
        return true;
    default:
        return false;
    }
}

static esp_err_t app_xiaozhi_apply_light_set_state(
    const xiaozhi_foundation_light_set_state_request_t *request,
    xiaozhi_foundation_light_set_state_result_t *result,
    void *user_context)
{
    (void)user_context;
    if ((request == NULL) || (result == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *result = (xiaozhi_foundation_light_set_state_result_t){
        .outcome = XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SNAPSHOT_FAILED,
    };

    light_manager_state_t current = {0};
    esp_err_t ret = light_manager_get_state(&current);
    if (ret != ESP_OK) {
        result->outcome = app_xiaozhi_light_outcome_from_error(
            ret,
            XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SNAPSHOT_FAILED);
        return ESP_OK;
    }

    light_manager_state_t requested = current;
    if (request->has_power) {
        requested.power_on = request->power_on;
    }
    if (request->has_color) {
        requested.red = request->red;
        requested.green = request->green;
        requested.blue = request->blue;
    }
    if (request->has_brightness) {
        requested.brightness_percent = request->brightness_percent;
    }
    if (request->has_effect &&
        !app_xiaozhi_light_effect_to_manager(request->effect, &requested.effect)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->has_effect) {
        /* An effect command is an activation request. The parser rejects an
         * explicit power=off, so an omitted power value must not preserve an
         * invisible OFF state. A black boot/custom state is equally invisible,
         * therefore use the product neutral white fallback only when no color
         * was requested. */
        if (!request->has_power) {
            requested.power_on = true;
        }
        if (!request->has_color && app_xiaozhi_light_color_is_black(&requested)) {
            requested.red = APP_XIAOZHI_LIGHT_EFFECT_DEFAULT_RED;
            requested.green = APP_XIAOZHI_LIGHT_EFFECT_DEFAULT_GREEN;
            requested.blue = APP_XIAOZHI_LIGHT_EFFECT_DEFAULT_BLUE;
        }
    }

    ret = light_manager_set_state(&requested);
    if (ret != ESP_OK) {
        result->outcome = app_xiaozhi_light_outcome_from_error(
            ret,
            XIAOZHI_FOUNDATION_LIGHT_SET_STATE_APPLY_FAILED);
        return ESP_OK;
    }

    light_manager_state_t applied = {0};
    ret = light_manager_get_state(&applied);
    if (ret != ESP_OK) {
        result->outcome = app_xiaozhi_light_outcome_from_error(
            ret,
            XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SNAPSHOT_FAILED);
        return ESP_OK;
    }

    result->outcome = XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SUCCESS;
    result->power_on = applied.power_on;
    result->red = applied.red;
    result->green = applied.green;
    result->blue = applied.blue;
    result->brightness_percent = applied.brightness_percent;
    switch (applied.effect) {
    case LIGHT_MANAGER_EFFECT_SOLID:
        result->effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_SOLID;
        break;
    case LIGHT_MANAGER_EFFECT_BLINK:
        result->effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_BLINK;
        break;
    case LIGHT_MANAGER_EFFECT_BREATH:
        result->effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_BREATH;
        break;
    case LIGHT_MANAGER_EFFECT_PULSE:
        result->effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_PULSE;
        break;
    case LIGHT_MANAGER_EFFECT_RAINBOW:
        result->effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_RAINBOW;
        break;
    default:
        result->outcome = XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SNAPSHOT_FAILED;
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t app_xiaozhi_light_set_state_register_provider(void)
{
    return xiaozhi_foundation_register_light_set_state_provider(
        app_xiaozhi_apply_light_set_state,
        NULL);
}
