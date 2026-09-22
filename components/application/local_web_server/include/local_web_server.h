#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the local bounded Storage Web frontend.
 *
 * This prepares no Wi-Fi, SD, or LVGL resource. It is idempotent after a
 * successful call and must run before local_web_server_start().
 */
esp_err_t local_web_server_init(void);

/**
 * @brief Start the bounded HTTP server after the application is online.
 *
 * The server is a presentation layer only. It calls public
 * `sd_card_manager` APIs for bounded metadata and transfer requests, but does
 * not mount/unmount SD, retain VFS handles, call LVGL, or control Wi-Fi. This
 * task-context API is idempotent after success.
 */
esp_err_t local_web_server_start(void);

#ifdef __cplusplus
}
#endif
