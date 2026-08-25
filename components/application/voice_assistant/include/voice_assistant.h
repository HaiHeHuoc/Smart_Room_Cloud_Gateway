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

/**
 * Project-owned copy of the audio-manager lifecycle relevant to voice work.
 *
 * This deliberately does not expose I2S handles, DMA storage, PCM pointers,
 * audio_manager enums, or hardware-driver state. The application composition
 * layer may translate one audio_manager status snapshot into this type.
 */
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

/**
 * Copied project-owned state safe for application/UI adapters.
 *
 * This is the Phase-13 GUI contract. UI code can map these scalars into an
 * app_gui model without depending on Xiaozhi, audio_manager, I2S, or LVGL
 * objects from this component.
 */
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
 * The callback runs from voice-assistant task/caller context after the internal
 * status mutex has been released. Keep it short; copy/queue before returning.
 * It must not call LVGL directly.
 */
typedef void (*voice_assistant_status_callback_t)(
    const voice_assistant_status_t *status,
    void *user_context);

/** Initialize bounded synchronization and register the Xiaozhi session observer. */
esp_err_t voice_assistant_init(void);

/** Start the single voice-assistant orchestration task and enter IDLE. */
esp_err_t voice_assistant_start(void);

/**
 * Begin one logical conversation session asynchronously.
 *
 * The task advances to CONNECTING, starts the project-owned Xiaozhi WebSocket
 * session, and reaches READY only after the foundation receives a real
 * CONNECTED event. No microphone, speaker, or audio channel is started here.
 */
esp_err_t voice_assistant_begin_session(void);

/**
 * Stop the active Xiaozhi transport session asynchronously and return to IDLE.
 *
 * This does not own or stop audio hardware. audio_manager remains the sole
 * I2S/DMA/PCM owner.
 */
esp_err_t voice_assistant_end_session(void);

/**
 * Queue one copied audio-manager-facing snapshot into voice orchestration.
 *
 * The call is non-blocking and does not take I2S ownership. Phase 13-C records
 * audio readiness/activity only; it does not infer LISTENING/THINKING/SPEAKING
 * merely because an unrelated audio operation is active.
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
