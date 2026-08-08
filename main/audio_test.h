#pragma once

#include <stddef.h>

#include "esp_err.h"

/**
 * @file audio_test.h
 * @brief Public API for the one-shot microphone capture diagnostic.
 *
 * The module owns its I2S receive channel and PSRAM capture buffer. Call its
 * APIs only from task context; it does not provide ISR-safe entry points or
 * concurrent capture support.
 */

#ifdef __cplusplus
extern "C"
{
#endif

/* Functions ---------------------------------------------------------------- */

/**
 * @brief Initialize the microphone capture diagnostic.
 *
 * Allocates the capture buffer and configures the speaker data pin to its safe
 * low state. Must complete successfully before recording.
 *
 * @return ESP_OK on success; otherwise an ESP-IDF error code.
 */
esp_err_t audio_test_init(void);

/**
 * @brief Capture one microphone sample buffer in the calling task.
 *
 * Blocks until the configured capture duration completes or I2S reports an
 * error. The caller retains ownership of @p samples_recorded.
 *
 * @param[out] samples_recorded Optional number of samples captured.
 *
 * @return ESP_OK on success; otherwise an ESP-IDF error code.
 */
esp_err_t audio_test_record_once(
    size_t *samples_recorded);

/**
 * @brief Start the audio test module.
 *
 * @return ESP_OK on success; otherwise an ESP-IDF error code.
 */
esp_err_t audio_test_start(void);

#ifdef __cplusplus
}
#endif
