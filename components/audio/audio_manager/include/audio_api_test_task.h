#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Start the continuous hardware stress coordinator for audio public APIs.
 *
 * The coordinator belongs to the audio_manager component but uses only public
 * audio_manager APIs plus sd_card_manager readiness. Test selection, task
 * priority, manual hold time, cycle delay, polling period, and WAV path are
 * intentionally configured with documented #defines at the top of
 * audio_api_test_task.c so hardware scenarios can be changed without adding
 * production Kconfig surface.
 *
 * @return ESP_OK when the task was created.
 * @return ESP_ERR_INVALID_STATE when the stress task is already active.
 * @return ESP_ERR_NO_MEM when the FreeRTOS task cannot be created.
 */
esp_err_t app_audio_api_test_task_start(void);

#ifdef __cplusplus
}
#endif
