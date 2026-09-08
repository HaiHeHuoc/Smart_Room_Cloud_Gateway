#include "xiaozhi_foundation.h"

#include "esp_xiaozhi_chat.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_text_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_text_callback_t s_text_callback = NULL;
static void *s_text_callback_context = NULL;
static esp_xiaozhi_chat_event_callback_t s_upstream_event_callback = NULL;
static void *s_upstream_event_context = NULL;

static void xiaozhi_text_publish(const esp_xiaozhi_chat_text_data_t *text_data)
{
    if ((text_data == NULL) || (text_data->text == NULL) ||
        (text_data->text[0] == '\0')) {
        return;
    }

    xiaozhi_foundation_text_callback_t callback = NULL;
    void *context = NULL;
    uint32_t generation = 0U;

    xiaozhi_foundation_session_status_t session = {0};
    if (xiaozhi_foundation_session_get_status(&session) == ESP_OK) {
        generation = session.client_generation;
    }

    portENTER_CRITICAL(&s_text_lock);
    callback = s_text_callback;
    context = s_text_callback_context;
    portEXIT_CRITICAL(&s_text_lock);

    if ((callback == NULL) || (generation == 0U)) {
        return;
    }

    xiaozhi_foundation_text_role_t role;
    switch (text_data->role) {
        case ESP_XIAOZHI_CHAT_TEXT_ROLE_USER:
            role = XIAOZHI_FOUNDATION_TEXT_ROLE_USER;
            break;
        case ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT:
            role = XIAOZHI_FOUNDATION_TEXT_ROLE_ASSISTANT;
            break;
        default:
            return;
    }

    const xiaozhi_foundation_text_event_t event = {
        .role = role,
        .client_generation = generation,
        .text = text_data->text,
    };
    callback(&event, context);
}

static void xiaozhi_text_event_bridge(
    esp_xiaozhi_chat_event_t event,
    void *event_data,
    void *ctx)
{
    (void)ctx;

    esp_xiaozhi_chat_event_callback_t upstream = NULL;
    void *upstream_context = NULL;

    portENTER_CRITICAL(&s_text_lock);
    upstream = s_upstream_event_callback;
    upstream_context = s_upstream_event_context;
    portEXIT_CRITICAL(&s_text_lock);

    if (upstream != NULL) {
        upstream(event, event_data, upstream_context);
    }

    if (event == ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT) {
        xiaozhi_text_publish((const esp_xiaozhi_chat_text_data_t *)event_data);
    }
}

esp_err_t xiaozhi_foundation_text_register_callback(
    xiaozhi_foundation_text_callback_t callback,
    void *user_context)
{
    portENTER_CRITICAL(&s_text_lock);
    s_text_callback = callback;
    s_text_callback_context = user_context;
    portEXIT_CRITICAL(&s_text_lock);
    return ESP_OK;
}

/*
 * Source-local redirect target used only by xiaozhi_session.c. The wrapper
 * preserves every upstream protocol event and adds project-owned semantic text
 * observation without changing the transport/session state machine.
 */
esp_err_t xiaozhi_foundation_chat_init_bridge(
    esp_xiaozhi_chat_config_t *config,
    esp_xiaozhi_chat_handle_t *chat_hd)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_xiaozhi_chat_config_t bridged = *config;

    portENTER_CRITICAL(&s_text_lock);
    s_upstream_event_callback = config->event_callback;
    s_upstream_event_context = config->event_callback_ctx;
    portEXIT_CRITICAL(&s_text_lock);

    bridged.event_callback = xiaozhi_text_event_bridge;
    bridged.event_callback_ctx = NULL;

    return esp_xiaozhi_chat_init(&bridged, chat_hd);
}
