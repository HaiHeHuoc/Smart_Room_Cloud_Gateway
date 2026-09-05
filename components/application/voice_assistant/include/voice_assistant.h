#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Application-owned conversation lifecycle state. */
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

/** Project-owned copy of audio-manager lifecycle relevant to voice work. */
typedef enum {
    VOICE_ASSISTANT_AUDIO_UNAVAILABLE = 0,
    VOICE_ASSISTANT_AUDIO_INITIALIZED,
    VOICE_ASSISTANT_AUDIO_IDLE,
    VOICE_ASSISTANT_AUDIO_RECORDING,
    VOICE_ASSISTANT_AUDIO_PROCESSING,
    VOICE_ASSISTANT_AUDIO_PLAYBACK,
    VOICE_ASSISTANT_AUDIO_ERROR,
} voice_assistant_audio_state_t;

/** Bounded copied audio facts consumed only by the voice orchestration task. */
typedef struct {
    voice_assistant_audio_state_t state;
    bool capture_active;
    bool playback_active;
    esp_err_t last_error;
} voice_assistant_audio_status_t;

/** Copied project-owned state safe for application/UI adapters. */
typedef struct {
    voice_assistant_state_t state;
    uint32_t session_generation;
    bool session_active;
    esp_err_t last_error;
    voice_assistant_audio_status_t audio;
} voice_assistant_status_t;

/**
 * Receive one borrowed copied status snapshot.
 *
 * The callback runs after the internal status mutex has been released. Keep it
 * short; copy/queue before returning and never call LVGL directly.
 */
typedef void (*voice_assistant_status_callback_t)(
    const voice_assistant_status_t *status,
    void *user_context);

/** Initialize bounded synchronization and register the Xiaozhi session observer. */
esp_err_t voice_assistant_init(void);

/** Start the single voice-assistant orchestration task and enter IDLE. */
esp_err_t voice_assistant_start(void);

/** Begin one logical conversation session asynchronously. */
esp_err_t voice_assistant_begin_session(void);

/** Stop the active Xiaozhi transport session asynchronously and return to IDLE. */
esp_err_t voice_assistant_end_session(void);

/**
 * Recover explicitly from ERROR with bounded cleanup and no automatic reconnect.
 *
 * Recovery is asynchronous. The orchestration task enters RECOVERING, cleans a
 * still-active Xiaozhi session when necessary, and returns to IDLE on success.
 * This API deliberately does not loop or reconnect automatically.
 */
esp_err_t voice_assistant_recover(void);

/**
 * Queue one copied audio-manager-facing snapshot into voice orchestration.
 *
 * Audio updates are coalesced so rapid status publication cannot consume the
 * command queue needed by session/control events. The latest copied snapshot
 * wins while one audio-status marker is pending.
 */
esp_err_t voice_assistant_notify_audio_status(
    const voice_assistant_audio_status_t *status);

/** Register or remove the single copied-status observer. */
esp_err_t voice_assistant_register_status_callback(
    voice_assistant_status_callback_t callback,
    void *user_context);

/** Copy one internally consistent status snapshot. */
esp_err_t voice_assistant_get_status(voice_assistant_status_t *status);

/** Convert a conversation state into a stable diagnostic string. */
const char *voice_assistant_state_to_string(voice_assistant_state_t state);

/** Convert a project-owned audio state into a stable diagnostic string. */
const char *voice_assistant_audio_state_to_string(
    voice_assistant_audio_state_t state);

#ifdef __cplusplus
}
#endif
