#include "smart_room_mcp_adapter_internal.h"

#include "light_manager.h"
#include "xiaozhi_foundation.h"

static bool app_xiaozhi_light_effect_from_manager(
    light_manager_effect_t effect,
    xiaozhi_foundation_light_effect_t *foundation_effect)
{
    if (foundation_effect == NULL) {
        return false;
    }

    switch (effect) {
    case LIGHT_MANAGER_EFFECT_SOLID:
        *foundation_effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_SOLID;
        return true;
    case LIGHT_MANAGER_EFFECT_BLINK:
        *foundation_effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_BLINK;
        return true;
    case LIGHT_MANAGER_EFFECT_BREATH:
        *foundation_effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_BREATH;
        return true;
    case LIGHT_MANAGER_EFFECT_PULSE:
        *foundation_effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_PULSE;
        return true;
    case LIGHT_MANAGER_EFFECT_RAINBOW:
        *foundation_effect = XIAOZHI_FOUNDATION_LIGHT_EFFECT_RAINBOW;
        return true;
    default:
        return false;
    }
}

static esp_err_t app_xiaozhi_copy_light_state(
    xiaozhi_foundation_light_state_query_snapshot_t *snapshot,
    void *user_context)
{
    (void)user_context;
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = (xiaozhi_foundation_light_state_query_snapshot_t){0};
    light_manager_state_t state = {0};
    const esp_err_t ret = light_manager_get_state(&state);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!app_xiaozhi_light_effect_from_manager(state.effect, &snapshot->effect)) {
        return ESP_ERR_INVALID_STATE;
    }

    snapshot->available = true;
    snapshot->power_on = state.power_on;
    snapshot->red = state.red;
    snapshot->green = state.green;
    snapshot->blue = state.blue;
    snapshot->brightness_percent = state.brightness_percent;
    return ESP_OK;
}

esp_err_t smart_room_mcp_light_state_register_provider(void)
{
    return xiaozhi_foundation_register_light_state_query_provider(
        app_xiaozhi_copy_light_state,
        NULL);
}
