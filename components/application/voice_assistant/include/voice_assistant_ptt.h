#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum {
    VOICE_ASSISTANT_PTT_UNINITIALIZED = 0,
    VOICE_ASSISTANT_PTT_IDLE,
    VOICE_ASSISTANT_PTT_ARMING_SESSION,
    VOICE_ASSISTANT_PTT_AUTHORIZED,
    VOICE_ASSISTANT_PTT_RELEASED,
    VOICE_ASSISTANT_PTT_CANCEL_PENDING,
    VOICE_ASSISTANT_PTT_ERROR,
} voice_assistant_ptt_state_t;

typedef struct {
    voice_assistant_ptt_state_t state;
    uint32_t ptt_generation;
    uint32_t session_generation;
    bool pressed;
    bool capture_authorized;
    esp_err_t last_error;
} voice_assistant_ptt_status_t;

typedef void (*voice_assistant_ptt_status_callback_t)(
    const voice_assistant_ptt_status_t *status,
    void *user_context);

/** Initialize the bounded PTT policy queue/task state. Does not own GPIO. */
esp_err_t voice_assistant_ptt_init(void);

/** Start the PTT policy task and enter IDLE. */
esp_err_t voice_assistant_ptt_start(void);

/**
 * Queue an authorized-user press intent.
 *
 * If the production voice session is IDLE, the PTT policy asks
 * voice_assistant to begin one session and waits asynchronously for READY. If
 * it is already CONNECTING, the press is armed and waits for that same READY
 * evidence. From ERROR, one continuously held press is retained through
 * bounded recovery and starts a fresh session after IDLE. Capture authorization
 * becomes true only after real READY evidence exists. A press while a prior
 * turn is awaiting, receiving, finalizing, or playing its server response is
 * ignored and never reported as capture-authorized.
 */
esp_err_t voice_assistant_ptt_press(void);

/**
 * Queue release intent. Authorization is revoked immediately by the PTT task.
 * 14-A does not start/stop microphone capture; Phase 14-B consumes this policy.
 */
esp_err_t voice_assistant_ptt_release(void);

/**
 * Cancel the current PTT intent. If a transport start is still pending,
 * cancellation remains pending until the bounded start resolves, then revokes
 * capture authorization without closing the long-lived production session.
 */
esp_err_t voice_assistant_ptt_cancel(void);

/** Register/remove one copied PTT-status observer. */
esp_err_t voice_assistant_ptt_register_status_callback(
    voice_assistant_ptt_status_callback_t callback,
    void *user_context);

/** Copy the current PTT policy status. */
esp_err_t voice_assistant_ptt_get_status(voice_assistant_ptt_status_t *status);

const char *voice_assistant_ptt_state_to_string(voice_assistant_ptt_state_t state);

#ifdef __cplusplus
}
#endif
