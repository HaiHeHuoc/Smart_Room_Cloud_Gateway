#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copied project-runtime state for a live Xiaozhi microphone capture.
 *
 * This is deliberately not a PTT, audio-manager, or provider handle. The
 * voice uplink owner enters it only after the capture arbiter confirms actual
 * recording and leaves it after the bounded capture-stop path. Consumers may
 * inspect it to defer their own optional work; they must never use it to stop
 * a task or take ownership of a voice resource.
 */
typedef struct {
    bool active;
    uint32_t session_generation;
    uint32_t ptt_generation;
    uint32_t transition_sequence;
    uint32_t entered_count;
    uint32_t exited_count;
} voice_recording_critical_status_t;

/**
 * @brief Lightweight task-context notification of a critical-window edge.
 *
 * A listener runs in the voice-uplink task after the runtime lock is released.
 * It must not block, log, allocate, or perform I/O; task notification is the
 * intended use. Registration is static for component lifetime.
 */
typedef void (*voice_recording_critical_listener_t)(
    bool active,
    uint32_t transition_sequence,
    void *context);

/**
 * @brief Enter the live-recording window for one already-started capture.
 *
 * The single voice-uplink owner calls this only after successful capture
 * admission. Repeating the same generation is idempotent; another live
 * generation is rejected rather than overwriting the current owner.
 */
esp_err_t voice_recording_critical_enter(
    uint32_t session_generation,
    uint32_t ptt_generation);

/**
 * @brief Exit the live-recording window for its owning generation.
 *
 * Exiting an already-idle state is idempotent. A stale different generation is
 * rejected so an old cleanup path cannot clear a newer capture window.
 */
esp_err_t voice_recording_critical_exit(
    uint32_t session_generation,
    uint32_t ptt_generation);

/** @brief Non-blocking copied snapshot for cooperative background policy. */
void voice_recording_critical_get_status(
    voice_recording_critical_status_t *status);

/** @brief Convenience non-blocking query for optional-work safe points. */
bool voice_recording_critical_is_active(void);

/**
 * @brief Register one static lightweight listener for state transitions.
 *
 * Re-registering the same callback/context pair is idempotent. This is setup
 * only; callers must not register or unregister on every PTT turn.
 */
esp_err_t voice_recording_critical_register_listener(
    voice_recording_critical_listener_t listener,
    void *context);

#ifdef __cplusplus
}
#endif
