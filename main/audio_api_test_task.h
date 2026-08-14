#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Start one-shot validation of the public audio_manager APIs.
 *
 * The task runs at priority 6 and calls only public audio_manager and
 * sd_card_manager APIs. It validates fixed recording, retained playback,
 * manual recording/stop, retained playback again, then WAV playback.
 *
 * @return ESP_OK when the task was created.
 * @return ESP_ERR_INVALID_STATE when a validation task is already active.
 * @return ESP_ERR_NO_MEM when the FreeRTOS task cannot be created.
 */
esp_err_t app_audio_api_test_task_start(void);

#ifdef __cplusplus
}
#endif
