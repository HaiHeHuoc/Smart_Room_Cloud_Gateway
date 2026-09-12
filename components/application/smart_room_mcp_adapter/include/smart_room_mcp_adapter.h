#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the bounded Smart Room MCP providers with Xiaozhi Foundation.
 *
 * This composition-owned call installs the existing sensor, cloud-sync,
 * system-status, logical-light-state, and controlled-light-state providers.
 * It must run after their manager owners are initialized and before the
 * production voice session is started. Provider callbacks are borrowed for
 * firmware lifetime and only copy public manager snapshots or invoke the
 * logical light-manager API; they never access Xiaozhi transport handles.
 *
 * @return ESP_OK when every provider was registered, otherwise the first
 *         registration error. Registration has no hardware side effects.
 */
esp_err_t smart_room_mcp_adapter_register_providers(void);

#ifdef __cplusplus
}
#endif
