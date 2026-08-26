#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Phase-14 bounded server-response aggregation status. */
typedef struct {
    bool initialized;
    bool running;
    bool collecting;
    bool playback_requested;
    uint32_t session_generation;
    /** Encoded Opus bytes copied from complete response packet callbacks. */
    uint64_t response_bytes_received;
    /** Decoded PCM16 bytes currently buffered for the WAV handoff. */
    uint64_t response_bytes_buffered;
    uint32_t chunks_queued;
    uint32_t chunks_dropped_queue_full;
    uint32_t chunks_dropped_stale;
    uint32_t response_timeouts;
    uint32_t responses_completed;
    uint32_t responses_failed;
    esp_err_t last_error;
} voice_assistant_downlink_status_t;

/** Allocate the bounded downlink queue and PSRAM response buffer. */
esp_err_t voice_assistant_downlink_init(void);

/** Register the Xiaozhi response callback and start the downlink worker. */
esp_err_t voice_assistant_downlink_start(void);

/** Copy current scalar diagnostics. */
esp_err_t voice_assistant_downlink_get_status(
    voice_assistant_downlink_status_t *status);

/**
 * @brief True while a prior PTT turn is still receiving or playing a response.
 *
 * Higher-level turn coordination uses this to serialize repeated turns. This
 * function only reads copied project state; it never touches Xiaozhi or I2S.
 */
bool voice_assistant_downlink_is_busy(void);

#ifdef __cplusplus
}
#endif
