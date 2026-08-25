#include "voice_assistant_ui_gui_adapter.h"

#include <string.h>

#include "app_gui.h"
#include "esp_log.h"
#include "voice_assistant_ui_model.h"

static const char *const TAG = "VOICE_UI_GUI";

static bool s_initialized = false;
static bool s_started = false;

static ui_xiaozhi_state_t gui_map_state(voice_assistant_ui_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_UI_READY:
            return UI_XIAOZHI_STATE_READY;
        case VOICE_ASSISTANT_UI_LISTENING:
            return UI_XIAOZHI_STATE_LISTENING;
        case VOICE_ASSISTANT_UI_SPEAKING:
            return UI_XIAOZHI_STATE_RESPONDING;
        case VOICE_ASSISTANT_UI_ERROR:
            return UI_XIAOZHI_STATE_ERROR;
        case VOICE_ASSISTANT_UI_CONNECTING:
        case VOICE_ASSISTANT_UI_THINKING:
        case VOICE_ASSISTANT_UI_RECOVERING:
            return UI_XIAOZHI_STATE_PROCESSING;
        case VOICE_ASSISTANT_UI_IDLE:
        default:
            return UI_XIAOZHI_STATE_DISCONNECTED;
    }
}

static bool gui_state_should_present(voice_assistant_ui_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_UI_CONNECTING:
        case VOICE_ASSISTANT_UI_READY:
        case VOICE_ASSISTANT_UI_LISTENING:
        case VOICE_ASSISTANT_UI_THINKING:
        case VOICE_ASSISTANT_UI_SPEAKING:
        case VOICE_ASSISTANT_UI_RECOVERING:
        case VOICE_ASSISTANT_UI_ERROR:
            return true;
        case VOICE_ASSISTANT_UI_IDLE:
        default:
            return false;
    }
}

static void gui_copy_text(
    char destination[UI_XIAOZHI_TEXT_BUFFER_SIZE],
    const char *source,
    bool valid)
{
    destination[0] = '\0';
    if (!valid || (source == NULL)) {
        return;
    }
    (void)snprintf(destination,
                   UI_XIAOZHI_TEXT_BUFFER_SIZE,
                   "%s",
                   source);
}

static void gui_model_callback(
    const voice_assistant_ui_model_t *model,
    void *user_context)
{
    (void)user_context;
    if (model == NULL) {
        return;
    }

    ui_xiaozhi_status_t gui = {
        .state = gui_map_state(model->state),
        .last_error = model->last_error,
        .user_text_truncated = model->user_text_truncated,
        .assistant_text_truncated = model->assistant_text_truncated,
    };

    gui_copy_text(gui.user_text, model->user_text, model->user_text_valid);
    gui_copy_text(gui.assistant_text,
                  model->assistant_text,
                  model->assistant_text_valid);

    const esp_err_t post_ret = app_gui_post_xiaozhi_status(&gui);
    if ((post_ret != ESP_OK) && (post_ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(TAG,
                 "voice GUI snapshot dropped state=%s error=%s",
                 voice_assistant_ui_state_to_string(model->state),
                 esp_err_to_name(post_ret));
    }

    if (!gui_state_should_present(model->state)) {
        return;
    }

    app_gui_screen_id_t current = APP_GUI_SCREEN_NONE;
    if ((app_gui_get_screen_id(&current) == ESP_OK) &&
        (current == APP_GUI_SCREEN_XIAOZHI)) {
        return;
    }

    const esp_err_t screen_ret =
        app_gui_request_screen(APP_GUI_SCREEN_XIAOZHI);
    if ((screen_ret != ESP_OK) &&
        (screen_ret != ESP_ERR_INVALID_STATE) &&
        (screen_ret != ESP_ERR_TIMEOUT)) {
        ESP_LOGW(TAG,
                 "voice screen request failed state=%s error=%s",
                 voice_assistant_ui_state_to_string(model->state),
                 esp_err_to_name(screen_ret));
    }
}

esp_err_t voice_assistant_ui_gui_adapter_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t voice_assistant_ui_gui_adapter_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    const esp_err_t ret = voice_assistant_ui_model_register_callback(
        gui_model_callback,
        NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    voice_assistant_ui_model_t model = {0};
    const esp_err_t get_ret = voice_assistant_ui_model_get(&model);
    if (get_ret != ESP_OK) {
        (void)voice_assistant_ui_model_register_callback(NULL, NULL);
        return get_ret;
    }

    s_started = true;
    gui_model_callback(&model, NULL);
    ESP_LOGI(TAG, "production voice GUI adapter started");
    return ESP_OK;
}
