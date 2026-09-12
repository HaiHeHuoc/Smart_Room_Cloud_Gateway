#include "smart_room_mcp_adapter_internal.h"

#include <math.h>

#include "sensor_manager.h"
#include "xiaozhi_foundation.h"

static esp_err_t app_xiaozhi_copy_current_sensor(
    xiaozhi_foundation_sensor_query_snapshot_t *snapshot,
    void *user_context)
{
    (void)user_context;
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = (xiaozhi_foundation_sensor_query_snapshot_t){0};
    sensor_manager_status_t sensor = {0};
    const esp_err_t ret = sensor_manager_get_status(&sensor);
    if (ret != ESP_OK) {
        return ret;
    }

    const bool valid = sensor.data_valid && !sensor.data_stale &&
        isfinite(sensor.last_valid_temperature_c) &&
        isfinite(sensor.last_valid_humidity_percent);
    if (valid) {
        snapshot->available = true;
        snapshot->temperature_c = sensor.last_valid_temperature_c;
        snapshot->humidity_percent = sensor.last_valid_humidity_percent;
    }
    return ESP_OK;
}

esp_err_t smart_room_mcp_sensor_register_provider(void)
{
    return xiaozhi_foundation_register_sensor_query_provider(
        app_xiaozhi_copy_current_sensor,
        NULL);
}
