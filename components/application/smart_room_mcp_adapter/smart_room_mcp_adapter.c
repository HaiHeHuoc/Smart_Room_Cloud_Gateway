#include "smart_room_mcp_adapter.h"

#include "smart_room_mcp_adapter_internal.h"

esp_err_t smart_room_mcp_adapter_register_providers(void)
{
    esp_err_t ret = smart_room_mcp_sensor_register_provider();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = smart_room_mcp_light_set_state_register_provider();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = smart_room_mcp_light_state_register_provider();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = smart_room_mcp_cloud_sync_register_provider();
    if (ret != ESP_OK) {
        return ret;
    }

    return smart_room_mcp_system_status_register_provider();
}
