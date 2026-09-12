#pragma once

#include "esp_err.h"

/** Register the main-owned, read-only system-status snapshot provider for Xiaozhi. */
esp_err_t app_xiaozhi_system_status_query_register_provider(void);
