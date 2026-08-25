#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "voice_assistant.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define VOICE_ASSISTANT_UI_TEXT_BUFFER_SIZE 192U

typedef enum {
    VOICE_ASSISTANT_UI_IDLE = 0,
    VOICE_ASSISTANT_UI_CONNECTING,
    VOICE_ASSISTANT_UI_READY,
    VOICE_ASSISTANT_UI_LISTENING,
    VOICE_ASSISTANT_UI_THINKING,
    VOICE_ASSISTANT_UI_SPEAKING,
    VOICE_ASSISTANT_UI_RECOVERING,
    VOICE_ASSISTANT_UI_ERROR,
} voice_assistant_ui_state_t;

typedef struct {
    voice_assistant_ui_state_t state;
    uint32_t session_generation;
    esp_err_t last_error;

    bool user_text_valid;
    bool user_text_truncated;
    char user_text[VOICE_ASSISTANT_UI_TEXT_BUFFER_SIZE];

    bool assistant_text_valid;
    bool assistant_text_truncated;
    char assistant_text[VOICE_ASSISTANT_UI_TEXT_BUFFER_SIZE];
} voice_assistant_ui_model_t;

typedef void (*voice_assistant_ui_model_callback_t)(
    const voice_assistant_ui_model_t *model,
    void *user_context);

/** Initialize the production voice-presentation model. */
esp_err_t voice_assistant_ui_model_init(void);

/** Start observing copied voice_assistant status snapshots. */
esp_err_t voice_assistant_ui_model_start(void);

/** Register/remove the single presentation observer. */
esp_err_t voice_assistant_ui_model_register_callback(
    voice_assistant_ui_model_callback_t callback,
    void *user_context);

/** Copy the latest presentation-safe model. */
esp_err_t voice_assistant_ui_model_get(voice_assistant_ui_model_t *model);

/**
 * Copy one production user transcript into the presentation model.
 *
 * The input string is borrowed only for the duration of this call. Phase 15-C
 * wires Xiaozhi CHAT_TEXT/USER events to this ingress.
 */
esp_err_t voice_assistant_ui_model_post_user_text(
    uint32_t session_generation,
    const char *text);

/**
 * Copy one production assistant sentence into the presentation model.
 *
 * The input string is borrowed only for the duration of this call. Phase 15-D
 * wires Xiaozhi CHAT_TEXT/ASSISTANT events to this ingress.
 */
esp_err_t voice_assistant_ui_model_post_assistant_text(
    uint32_t session_generation,
    const char *text);

/** Clear conversation text while retaining the current lifecycle state. */
esp_err_t voice_assistant_ui_model_clear_text(void);

const char *voice_assistant_ui_state_to_string(voice_assistant_ui_state_t state);

#ifdef __cplusplus
}
#endif
