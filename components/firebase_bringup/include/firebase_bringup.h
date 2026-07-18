#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send one fixed HTTPS PUT request to Firebase.
 *
 * This is a temporary bring-up function.
 * It must be called from a normal FreeRTOS task after Wi-Fi has an IP address.
 */
esp_err_t firebase_bringup_put_test(void);

#ifdef __cplusplus
}
#endif
