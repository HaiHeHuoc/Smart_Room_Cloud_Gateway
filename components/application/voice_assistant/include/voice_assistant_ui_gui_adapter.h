#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * Initialize the production voice-presentation -> app_gui adapter.
 *
 * This adapter never calls LVGL. It only posts copied snapshots and screen
 * requests into app_gui's existing queues.
 */
esp_err_t voice_assistant_ui_gui_adapter_init(void);

/**
 * Start observing voice_assistant_ui_model snapshots.
 *
 * The adapter promotes the existing Xiaozhi visual surface for production
 * voice presentation while keeping Phase-12 validation data types outside the
 * voice model itself.
 */
esp_err_t voice_assistant_ui_gui_adapter_start(void);

#ifdef __cplusplus
}
#endif
