#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_manager_arbitration.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Playback arbiter lifecycle/state visible to application clients. */
typedef enum {
    AUDIO_MANAGER_PLAYBACK_ARBITER_UNINITIALIZED = 0,
    AUDIO_MANAGER_PLAYBACK_ARBITER_IDLE,
    AUDIO_MANAGER_PLAYBACK_ARBITER_STARTING,
    AUDIO_MANAGER_PLAYBACK_ARBITER_ACTIVE,
    AUDIO_MANAGER_PLAYBACK_ARBITER_PREEMPTING,
    AUDIO_MANAGER_PLAYBACK_ARBITER_ERROR,
} audio_manager_playback_arbiter_state_t;

/** Copied playback arbiter status. */
typedef struct {
    audio_manager_playback_arbiter_state_t state;
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
} audio_manager_playback_arbiter_status_t;

/** Initialize the playback arbitration runtime. Idempotent. */
esp_err_t audio_manager_playback_arbiter_init(void);

/** Start the arbiter task. Idempotent after init. */
esp_err_t audio_manager_playback_arbiter_start(void);

/**
 * Submit one WAV playback request.
 *
 * The request resource must be PLAYBACK and request_id must be non-zero.
 * path is copied before return. This API never opens the file or touches I2S.
 *
 * Busy behavior follows request.busy_policy:
 * - REJECT: reject while another arbiter request owns playback;
 * - QUEUE: accept into the single bounded pending slot when empty;
 * - PREEMPT_LOWER_PRIORITY: queue as replacement only when current owner is
 *   interruptible and has lower priority; cooperative stop is performed by the
 *   arbiter task through audio_manager_stop_playback().
 *
 * Playback already started outside this arbiter is treated as external busy
 * and is never preempted because its client/interruptibility metadata is
 * unknown.
 */
esp_err_t audio_manager_playback_arbiter_submit_wav(
    const audio_manager_request_t *request,
    const char *path);

/**
 * Cancel the current or pending request matching request_id.
 * Current playback cancellation is cooperative through audio_manager.
 */
esp_err_t audio_manager_playback_arbiter_cancel(uint32_t request_id);

/** Copy current arbitration status. */
esp_err_t audio_manager_playback_arbiter_get_status(
    audio_manager_playback_arbiter_status_t *status);

const char *audio_manager_playback_arbiter_state_to_string(
    audio_manager_playback_arbiter_state_t state);

#ifdef __cplusplus
}
#endif
