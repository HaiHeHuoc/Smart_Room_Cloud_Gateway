#pragma once

#include "esp_err.h"

/** Register the main-owned read-only light snapshot provider for Xiaozhi MCP. */
esp_err_t app_xiaozhi_light_state_query_register_provider(void);
