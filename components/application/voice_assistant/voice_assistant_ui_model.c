#include "voice_assistant_ui_model.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "xiaozhi_foundation.h"

#define VOICE_UI_LOCK_TIMEOUT_MS 100U

static const char *const TAG = "VOICE_UI_MODEL";

static SemaphoreHandle_t s_lock = NULL;
static bool s_started = false;
static voice_assistant_ui_model_t s_model = {
    .state = VOICE_ASSISTANT_UI_IDLE,
    .last_error = ESP_OK,
};
static voice_assistant_ui_model_callback_t s_callback = NULL;
static void *s_callback_context = NULL;

static bool ui_take_lock(void)
{
    return (s_lock != NULL) &&
           (xSemaphoreTake(s_lock, pdMS_TO_TICKS(VOICE_UI_LOCK_TIMEOUT_MS)) == pdTRUE);
}

static voice_assistant_ui_state_t ui_map_voice_state(voice_assistant_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_STATE_CONNECTING:
            return VOICE_ASSISTANT_UI_CONNECTING;
        case VOICE_ASSISTANT_STATE_READY:
            return VOICE_ASSISTANT_UI_READY;
        case VOICE_ASSISTANT_STATE_LISTENING:
            return VOICE_ASSISTANT_UI_LISTENING;
        case VOICE_ASSISTANT_STATE_THINKING:
            return VOICE_ASSISTANT_UI_THINKING;
        case VOICE_ASSISTANT_STATE_SPEAKING:
            return VOICE_ASSISTANT_UI_SPEAKING;
        case VOICE_ASSISTANT_STATE_RECOVERING:
            return VOICE_ASSISTANT_UI_RECOVERING;
        case VOICE_ASSISTANT_STATE_ERROR:
            return VOICE_ASSISTANT_UI_ERROR;
        case VOICE_ASSISTANT_STATE_UNINITIALIZED:
        case VOICE_ASSISTANT_STATE_INITIALIZED:
        case VOICE_ASSISTANT_STATE_IDLE:
        default:
            return VOICE_ASSISTANT_UI_IDLE;
    }
}

static void ui_clear_text_locked(void)
{
    s_model.user_text[0] = '\0';
    s_model.assistant_text[0] = '\0';
    s_model.user_text_valid = false;
    s_model.assistant_text_valid = false;
    s_model.user_text_truncated = false;
    s_model.assistant_text_truncated = false;
}

static void ui_publish(void)
{
    voice_assistant_ui_model_t snapshot = {0};
    voice_assistant_ui_model_callback_t callback = NULL;
    void *context = NULL;

    if (!ui_take_lock()) {
        ESP_LOGW(TAG, "publish skipped: lock timeout");
        return;
    }
    snapshot = s_model;
    callback = s_callback;
    context = s_callback_context;
    xSemaphoreGive(s_lock);

    if (callback != NULL) {
        callback(&snapshot, context);
    }
}

static void ui_voice_status_callback(
    const voice_assistant_status_t *status,
    void *user_context)
{
    (void)user_context;
    if (status == NULL) {
        return;
    }

    if (!ui_take_lock()) {
        ESP_LOGW(TAG, "voice status dropped: lock timeout");
        return;
    }
    if ((status->session_generation != 0U) &&
        (status->session_generation != s_model.session_generation)) {
        ui_clear_text_locked();
    }
    s_model.state = ui_map_voice_state(status->state);
    s_model.session_generation = status->session_generation;
    s_model.last_error = status->last_error;
    xSemaphoreGive(s_lock);
    ui_publish();
}

static esp_err_t ui_copy_text(
    uint32_t session_generation,
    const char *text,
    bool user_text)
{
    if ((session_generation == 0U) || (text == NULL) || (text[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t source_len =
        strnlen(text, VOICE_ASSISTANT_UI_TEXT_BUFFER_SIZE);
    const bool truncated =
        source_len == VOICE_ASSISTANT_UI_TEXT_BUFFER_SIZE;
    const size_t copy_len = truncated
                                ? (VOICE_ASSISTANT_UI_TEXT_BUFFER_SIZE - 1U)
                                : source_len;

    if (!ui_take_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    if ((s_model.session_generation == 0U) ||
        (s_model.session_generation != session_generation)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    char *const destination = user_text ? s_model.user_text : s_model.assistant_text;
    memcpy(destination, text, copy_len);
    destination[copy_len] = '\0';

    if (user_text) {
        s_model.user_text_valid = true;
        s_model.user_text_truncated = truncated;
    } else {
        s_model.assistant_text_valid = true;
        s_model.assistant_text_truncated = truncated;
    }
    xSemaphoreGive(s_lock);

    ui_publish();
    return ESP_OK;
}

static void ui_semantic_text_callback(
    const xiaozhi_foundation_text_event_t *event,
    void *user_context)
{
    (void)user_context;
    if ((event == NULL) ||
        (event->role != XIAOZHI_FOUNDATION_TEXT_ROLE_USER)) {
        return;
    }

    const esp_err_t ret = voice_assistant_ui_model_post_user_text(
        event->client_generation,
        event->text);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(TAG,
                 "USER transcript dropped generation=%u error=%s",
                 (unsigned)event->client_generation,
                 esp_err_to_name(ret));
    }
}

esp_err_t voice_assistant_ui_model_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!ui_take_lock()) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_TIMEOUT;
    }
    memset(&s_model, 0, sizeof(s_model));
    s_model.state = VOICE_ASSISTANT_UI_IDLE;
    s_model.last_error = ESP_OK;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_ui_model_start(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t ret = voice_assistant_register_status_callback(
        ui_voice_status_callback,
        NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = xiaozhi_foundation_text_register_callback(
        ui_semantic_text_callback,
        NULL);
    if (ret != ESP_OK) {
        (void)voice_assistant_register_status_callback(NULL, NULL);
        return ret;
    }

    voice_assistant_status_t status = {0};
    const esp_err_t get_ret = voice_assistant_get_status(&status);
    if (get_ret != ESP_OK) {
        (void)xiaozhi_foundation_text_register_callback(NULL, NULL);
        (void)voice_assistant_register_status_callback(NULL, NULL);
        return get_ret;
    }

    s_started = true;
    ui_voice_status_callback(&status, NULL);
    ESP_LOGI(TAG, "production voice UI model started with USER transcript observer");
    return ESP_OK;
}

esp_err_t voice_assistant_ui_model_register_callback(
    voice_assistant_ui_model_callback_t callback,
    void *user_context)
{
    if (!ui_take_lock()) {
        return (s_lock == NULL) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }
    s_callback = callback;
    s_callback_context = user_context;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_ui_model_get(voice_assistant_ui_model_t *model)
{
    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ui_take_lock()) {
        return (s_lock == NULL) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }
    *model = s_model;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_ui_model_post_user_text(
    uint32_t session_generation,
    const char *text)
{
    return ui_copy_text(session_generation, text, true);
}

esp_err_t voice_assistant_ui_model_post_assistant_text(
    uint32_t session_generation,
    const char *text)
{
    return ui_copy_text(session_generation, text, false);
}

esp_err_t voice_assistant_ui_model_clear_text(void)
{
    if (!ui_take_lock()) {
        return (s_lock == NULL) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
    }
    ui_clear_text_locked();
    xSemaphoreGive(s_lock);
    ui_publish();
    return ESP_OK;
}

const char *voice_assistant_ui_state_to_string(voice_assistant_ui_state_t state)
{
    switch (state) {
        case VOICE_ASSISTANT_UI_IDLE: return "IDLE";
        case VOICE_ASSISTANT_UI_CONNECTING: return "CONNECTING";
        case VOICE_ASSISTANT_UI_READY: return "READY";
        case VOICE_ASSISTANT_UI_LISTENING: return "LISTENING";
        case VOICE_ASSISTANT_UI_THINKING: return "THINKING";
        case VOICE_ASSISTANT_UI_SPEAKING: return "SPEAKING";
        case VOICE_ASSISTANT_UI_RECOVERING: return "RECOVERING";
        case VOICE_ASSISTANT_UI_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
