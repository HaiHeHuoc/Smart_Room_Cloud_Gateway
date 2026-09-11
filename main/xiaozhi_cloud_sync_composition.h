#pragma once

#include "esp_err.h"

/** Register the main-owned, read-only cloud-sync snapshot provider for Xiaozhi. */
esp_err_t app_xiaozhi_cloud_sync_query_register_provider(void);
