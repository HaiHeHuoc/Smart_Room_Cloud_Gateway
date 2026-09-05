#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_manager_playback_arbiter.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reserve one Xiaozhi PCM16 downlink stream through Phase-16 arbitration. */
esp_err_t phase16_xiaozhi_stream_begin(void);

/**
 * Copy one decoded 16 kHz mono PCM16 packet into the selected stream.
 *
 * A full bounded ingress returns ESP_ERR_TIMEOUT without a partial copy.
 * Task-context producers must retain that packet and apply their own finite
 * backpressure policy before declaring the stream failed.
 */
esp_err_t phase16_xiaozhi_stream_write(
    const int16_t *samples,
    size_t sample_count);

/** Mark server TTS_STOP and allow the selected stream to drain. */
esp_err_t phase16_xiaozhi_stream_finish(void);

/** Mark the selected stream failed and flush its bounded ingress. */
esp_err_t phase16_xiaozhi_stream_fail(esp_err_t error);

/** Copy selected stream state without exposing the arbiter request id. */
esp_err_t phase16_xiaozhi_stream_get_status(
    audio_manager_playback_request_status_t *status);

/** Forget a stream after its terminal status has been consumed. */
void phase16_xiaozhi_stream_release(void);

#ifdef __cplusplus
}
#endif
