#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_manager_arbitration.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_MANAGER_CAPTURE_ARBITER_UNINITIALIZED = 0,
    AUDIO_MANAGER_CAPTURE_ARBITER_IDLE,
    AUDIO_MANAGER_CAPTURE_ARBITER_STARTING,
    AUDIO_MANAGER_CAPTURE_ARBITER_ACTIVE,
    AUDIO_MANAGER_CAPTURE_ARBITER_FINISHING,
    AUDIO_MANAGER_CAPTURE_ARBITER_PREEMPTING,
    AUDIO_MANAGER_CAPTURE_ARBITER_ERROR,
} audio_manager_capture_arbiter_state_t;

typedef struct {
    audio_manager_capture_arbiter_state_t state;
    bool current_valid;
    audio_manager_request_t current;
    bool pending_valid;
    audio_manager_request_t pending;
    esp_err_t last_error;
    uint32_t accepted_count;
    uint32_t rejected_count;
    uint32_t queued_count;
    uint32_t preemption_count;
    uint32_t completed_count;
    uint32_t failed_count;
} audio_manager_capture_arbiter_status_t;

esp_err_t audio_manager_capture_arbiter_init(void);
esp_err_t audio_manager_capture_arbiter_start(void);

/**
 * Submit one manual microphone-capture request.
 *
 * The request resource must be CAPTURE. The arbiter owns only request policy;
 * audio_manager remains the sole RX/I2S owner. Accepted capture is started via
 * audio_manager_start_recording() and cooperative stop/preemption uses
 * audio_manager_stop_recording(). One current and one pending request are
 * retained at most.
 */
esp_err_t audio_manager_capture_arbiter_submit(
    const audio_manager_request_t *request);

/** Stop/cancel the current request or remove the matching pending request. */
esp_err_t audio_manager_capture_arbiter_cancel(uint32_t request_id);

esp_err_t audio_manager_capture_arbiter_get_status(
    audio_manager_capture_arbiter_status_t *status);

const char *audio_manager_capture_arbiter_state_to_string(
    audio_manager_capture_arbiter_state_t state);

#ifdef __cplusplus
}
#endif
