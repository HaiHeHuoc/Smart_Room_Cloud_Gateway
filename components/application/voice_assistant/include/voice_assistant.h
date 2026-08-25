#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum {
    VOICE_ASSISTANT_STATE_UNINITIALIZED = 0,
    VOICE_ASSISTANT_STATE_INITIALIZED,
    VOICE_ASSISTANT_STATE_IDLE,
    VOICE_ASSISTANT_STATE_CONNECTING,
    VOICE_ASSISTANT_STATE_READY,
    VOICE_ASSISTANT_STATE_LISTENING,
    VOICE_ASSISTANT_STATE_THINKING,
    VOICE_ASSISTANT_STATE_SPEAKING,
    VOICE_ASSISTANT_STATE_RECOVERING,
    VOICE_ASSISTANT_STATE_ERROR,
} voice_assistant_state_t;

typedef struct {
    voice_assistant_state_t state;
    uint32_t session_generation;
    bool session_active;
    esp_err_t last_error;
} voice_assistant_status_t;

typedef void (*voice_assistant_status_callback_t)(
    const voice_assistant_status_t *status,
    void *user_context);

/** Initialize bounded synchronization and command infrastructure. */
esp_err_t voice_assistant_init(void);

/** Start the single voice-assistant orchestration task and enter IDLE. */
esp_err_t voice_assistant_start(void);

/**
 * Begin one logical conversation session.
 *
 * Phase 13-A only advances to CONNECTING. A later Xiaozhi adapter integration
 * owns the transport evidence required for CONNECTING -> READY.
 */
esp_err_t voice_assistant_begin_session(void);

/** End the current logical session and return the orchestrator to IDLE. */
esp_err_t voice_assistant_end_session(void);

/** Register or remove the single copied-status observer. */
esp_err_t voice_assistant_register_status_callback(
    voice_assistant_status_callback_t callback,
    void *user_context);

/** Copy one internally consistent status snapshot. */
esp_err_t voice_assistant_get_status(voice_assistant_status_t *status);

/** Convert a state into a stable diagnostic string. */
const char *voice_assistant_state_to_string(voice_assistant_state_t state);

#ifdef __cplusplus
}
#endif
