#include "xiaozhi_light_state_query_composition.h"

#include "light_manager.h"
#include "xiaozhi_foundation.h"

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

    snapshot->available = true;
    snapshot->power_on = state.power_on;
    snapshot->red = state.red;
    snapshot->green = state.green;
    snapshot->blue = state.blue;
    snapshot->brightness_percent = state.brightness_percent;
    return ESP_OK;
}

esp_err_t app_xiaozhi_light_state_query_register_provider(void)
{
    return xiaozhi_foundation_register_light_state_query_provider(
        app_xiaozhi_copy_light_state,
        NULL);
}
