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
    uint32_t turn_sequence;
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

esp_err_t voice_assistant_ui_model_init(void);
esp_err_t voice_assistant_ui_model_start(void);
esp_err_t voice_assistant_ui_model_register_callback(
    voice_assistant_ui_model_callback_t callback,
    void *user_context);
esp_err_t voice_assistant_ui_model_get(voice_assistant_ui_model_t *model);

/**
 * Copy one production user transcript. A valid USER semantic text starts the
 * next presentation turn within the current long-lived Xiaozhi session and
 * clears the previous assistant text before publishing the new user text.
 */
esp_err_t voice_assistant_ui_model_post_user_text(
    uint32_t session_generation,
    const char *text);

/** Copy one production assistant sentence into the current presentation turn. */
esp_err_t voice_assistant_ui_model_post_assistant_text(
    uint32_t session_generation,
    const char *text);

/** Clear current conversation text while retaining lifecycle/session state. */
esp_err_t voice_assistant_ui_model_clear_text(void);

const char *voice_assistant_ui_state_to_string(voice_assistant_ui_state_t state);

#ifdef __cplusplus
}
#endif
