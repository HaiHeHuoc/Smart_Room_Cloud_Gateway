#pragma once

#include "esp_err.h"

/** Register the main-owned controlled light adapter for Xiaozhi MCP. */
esp_err_t app_xiaozhi_light_set_state_register_provider(void);
