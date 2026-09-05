#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the bounded Phase-16 automated arbitration HIL coordinator.
 *
 * This test-branch-only coordinator is compiled into the application root but
 * is default-off through CONFIG_APP_PHASE16_AUTO_HIL_TEST.  It submits only
 * public arbitration requests and observes copied component status; it never
 * owns I2S, DMA, WAV file handles, or GPIO/PTT input.
 *
 * @return ESP_OK when the coordinator task was created.
 * @return ESP_ERR_INVALID_STATE when it is already running.
 * @return ESP_ERR_NO_MEM when task creation fails.
 * @return ESP_ERR_NOT_SUPPORTED when the Kconfig gate is disabled.
 */
esp_err_t app_phase16_auto_hil_test_start(void);

#ifdef __cplusplus
}
#endif
