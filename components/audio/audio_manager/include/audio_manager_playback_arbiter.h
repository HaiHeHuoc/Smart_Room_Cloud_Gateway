#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/** Per-request state retained for the bounded PCM16 streaming path. */
typedef enum {
    AUDIO_MANAGER_PLAYBACK_REQUEST_PENDING = 0,
    AUDIO_MANAGER_PLAYBACK_REQUEST_STARTING,
    AUDIO_MANAGER_PLAYBACK_REQUEST_ACTIVE,
    AUDIO_MANAGER_PLAYBACK_REQUEST_DRAINING,
    AUDIO_MANAGER_PLAYBACK_REQUEST_COMPLETED,
    AUDIO_MANAGER_PLAYBACK_REQUEST_CANCELLED,
    AUDIO_MANAGER_PLAYBACK_REQUEST_PREEMPTED,
    AUDIO_MANAGER_PLAYBACK_REQUEST_FAILED,
} audio_manager_playback_request_state_t;

/**
 * @brief Scalar stream outcome/ingress diagnostics for one request.
 *
 * The ring, decoder buffers, I2S handles, and DMA staging remain private to
 * their owners. `result` is ESP_OK for normal completion and cooperative
 * cancellation/preemption; use `state` to distinguish those terminal cases.
 */
typedef struct {
    uint32_t request_id;
    audio_manager_playback_request_state_t state;
    esp_err_t result;
    uint64_t pcm_samples_accepted;
    uint64_t pcm_samples_played;
    uint32_t ingress_queue_high_water;
    uint32_t ingress_full_count;
    uint32_t starvation_count;
} audio_manager_playback_request_status_t;

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
 * Reserve a bounded 16 kHz mono signed-PCM16 stream through playback
 * arbitration. The request is accepted before playback starts so its producer
 * can prefill the manager-owned ring while another client drains. The caller
 * must subsequently write complete packets, then finish or cancel it.
 */
esp_err_t audio_manager_playback_arbiter_submit_pcm16_stream(
    const audio_manager_request_t *request);

/**
 * Copy one complete PCM16 packet into the selected stream's bounded ingress
 * ring. At most 960 samples are accepted per call. This never touches I2S;
 * a full ring returns ESP_ERR_TIMEOUT without a partial copy.
 */
esp_err_t audio_manager_playback_arbiter_write_pcm16(
    uint32_t request_id,
    const int16_t *samples,
    size_t sample_count);

/** Mark a stream's producer EOF and allow its manager-owned ring to drain. */
esp_err_t audio_manager_playback_arbiter_finish_pcm16_stream(
    uint32_t request_id);

/**
 * Abort a stream because its producer/protocol failed. Unlike generic cancel,
 * the retained terminal record is FAILED with the supplied non-OK error.
 */
esp_err_t audio_manager_playback_arbiter_fail_pcm16_stream(
    uint32_t request_id,
    esp_err_t error);

/**
 * Cancel the current or pending request matching request_id.
 * Current playback cancellation is cooperative through audio_manager.
 */
esp_err_t audio_manager_playback_arbiter_cancel(uint32_t request_id);

/** Copy current or bounded retained terminal state for one PCM16 request. */
esp_err_t audio_manager_playback_arbiter_get_request_status(
    uint32_t request_id,
    audio_manager_playback_request_status_t *status);

/** Copy current arbitration status. */
esp_err_t audio_manager_playback_arbiter_get_status(
    audio_manager_playback_arbiter_status_t *status);

const char *audio_manager_playback_arbiter_state_to_string(
    audio_manager_playback_arbiter_state_t state);

#ifdef __cplusplus
}
#endif
