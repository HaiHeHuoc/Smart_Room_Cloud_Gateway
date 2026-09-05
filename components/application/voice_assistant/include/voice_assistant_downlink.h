#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Phase-16.1 bounded server-response streaming status. */
typedef struct {
    bool initialized;
    bool running;
    /** A completed local uplink is waiting for the server's TTS_START/error. */
    bool awaiting_response;
    bool collecting;
    /** True after TTS_STOP while the manager-owned PCM stream drains. */
    bool finalizing;
    bool playback_requested;
    uint32_t session_generation;
    /** Encoded Opus bytes copied from complete response packet callbacks. */
    uint64_t response_bytes_received;
    /** Decoded PCM16 bytes accepted into the bounded manager-owned stream. */
    uint64_t response_bytes_buffered;
    uint32_t chunks_queued;
    uint32_t chunks_dropped_queue_full;
    uint32_t chunks_dropped_stale;
    uint32_t response_timeouts;
    uint32_t responses_completed;
    uint32_t responses_failed;
    esp_err_t last_error;
} voice_assistant_downlink_status_t;

/** Allocate the bounded copied-packet downlink queue. */
esp_err_t voice_assistant_downlink_init(void);

/** Register the Xiaozhi response callback and start the downlink worker. */
esp_err_t voice_assistant_downlink_start(void);

/**
 * @brief Reserve the shared audio channel for the response to a completed
 *        local uplink.
 *
 * Call this before sending stop-listening for a non-empty PTT turn. It makes
 * subsequent PTT presses busy until TTS starts, an error arrives, or the
 * bounded response timeout aborts the channel. Thread-safe; it does not call
 * Xiaozhi or I2S.
 */
esp_err_t voice_assistant_downlink_begin_response_wait(uint32_t session_generation);

/**
 * @brief Cancel a response wait whose stop-listening request failed.
 *
 * This changes copied downlink state only. The caller remains responsible for
 * closing the Xiaozhi audio channel when appropriate.
 */
esp_err_t voice_assistant_downlink_cancel_response_wait(
    uint32_t session_generation,
    esp_err_t error);

/** Copy current scalar diagnostics. */
esp_err_t voice_assistant_downlink_get_status(
    voice_assistant_downlink_status_t *status);

/**
 * @brief True while a prior PTT turn is awaiting, receiving, finalizing, or
 *        playing a response.
 *
 * Higher-level turn coordination uses this to serialize repeated turns. This
 * function only reads copied project state; it never touches Xiaozhi or I2S.
 */
bool voice_assistant_downlink_is_busy(void);

#ifdef __cplusplus
}
#endif
