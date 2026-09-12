#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start enabled target-hardware test coordinators after audio is ready.
 *
 * The normal configuration performs no work and returns ESP_OK. Test
 * coordinators are selected only by their explicit Kconfig gates and use
 * public application/audio APIs; this facade never owns I2S, DMA, raw audio,
 * GPIO, SD file handles, or production lifecycle state.
 */
esp_err_t app_hil_test_start_after_audio_ready(void);

#ifdef __cplusplus
}
#endif
