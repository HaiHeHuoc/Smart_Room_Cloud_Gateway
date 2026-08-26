#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct {
    bool running;
    bool turn_active;
    uint32_t session_generation;
    /** Copied audio-manager PCM frames accepted by the bounded queue. */
    uint64_t frames_queued;
    /** Complete 60-ms Opus packets successfully sent to Xiaozhi. */
    uint64_t frames_sent;
    uint64_t frames_dropped_queue_full;
    uint64_t frames_dropped_stale;
    esp_err_t last_error;
} voice_assistant_uplink_status_t;

/** Initialize the bounded Phase-14 microphone-uplink coordinator. */
esp_err_t voice_assistant_uplink_init(void);

/** Start the coordinator task; no microphone capture starts here. */
esp_err_t voice_assistant_uplink_start(void);

/** Copy current uplink diagnostics. */
esp_err_t voice_assistant_uplink_get_status(
    voice_assistant_uplink_status_t *status);

#ifdef __cplusplus
}
#endif
