#pragma once

#include "esp_err.h"

/** Register the main-owned, read-only sensor snapshot provider for Xiaozhi. */
esp_err_t app_xiaozhi_sensor_query_register_provider(void);
