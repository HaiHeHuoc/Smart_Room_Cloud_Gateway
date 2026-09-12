#pragma once

#include "esp_err.h"

esp_err_t smart_room_mcp_sensor_register_provider(void);
esp_err_t smart_room_mcp_cloud_sync_register_provider(void);
esp_err_t smart_room_mcp_system_status_register_provider(void);
esp_err_t smart_room_mcp_light_state_register_provider(void);
esp_err_t smart_room_mcp_light_set_state_register_provider(void);
