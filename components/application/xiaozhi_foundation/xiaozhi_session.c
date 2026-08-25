#include "xiaozhi_foundation.h"

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define XIAOZHI_SESSION_EVENT_CONNECTED     BIT0
#define XIAOZHI_SESSION_EVENT_DISCONNECTED  BIT1
#define XIAOZHI_SESSION_EVENT_GOODBYE       BIT2
#define XIAOZHI_SESSION_CONNECT_TIMEOUT_MS  15000U

#define XIAOZHI_SESSION_UPSTREAM_CHAT_LOG_TAG    "ESP_XIAOZHI_CHAT"
#define XIAOZHI_SESSION_UPSTREAM_MCP_MANAGER_TAG "esp_mcp_mgr"
#define XIAOZHI_SESSION_UPSTREAM_MCP_ENGINE_TAG  "esp_mcp_engine"

static const char *const TAG = "XZ_SESSION";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_session_status_t s_status = {
    .state = XIAOZHI_FOUNDATION_SESSION_STOPPED,
    .client_generation = 0U,
    .active = false,
    .last_error = ESP_OK,
};
static xiaozhi_foundation_session_status_callback_t s_status_callback = NULL;
static void *s_status_callback_context = NULL;
static bool s_lifecycle_busy = false;
static bool s_intentional_stop = false;

static EventGroupHandle_t s_events = NULL;
static esp_mcp_t *s_mcp = NULL;
static esp_xiaozhi_chat_handle_t s_chat = 0;
static bool s_chat_started = false;
static bool s_event_handler_registered = false;
static esp_event_handler_instance_t s_event_handler_instance = NULL;

#if CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
static bool s_payload_logs_suppressed = false;
static esp_log_level_t s_chat_log_level = ESP_LOG_INFO;
static esp_log_level_t s_mcp_manager_log_level = ESP_LOG_INFO;
static esp_log_level_t s_mcp_engine_log_level = ESP_LOG_INFO;
#endif

static void xiaozhi_session_publish_status(void)
{
    xiaozhi_foundation_session_status_t snapshot = {0};
    xiaozhi_foundation_session_status_callback_t callback = NULL;
    void *callback_context = NULL;

    portENTER_CRITICAL(&s_lock);
    snapshot = s_status;
    callback = s_status_callback;
    callback_context = s_status_callback_context;
    portEXIT_CRITICAL(&s_lock);

    if (callback != NULL) {
        callback(&snapshot, callback_context);
    }
}

static void xiaozhi_session_set_status(
    xiaozhi_foundation_session_state_t state,
    bool active,
    esp_err_t error)
{
    xiaozhi_foundation_session_state_t previous =
        XIAOZHI_FOUNDATION_SESSION_STOPPED;
    uint32_t generation = 0U;

    portENTER_CRITICAL(&s_lock);
    previous = s_status.state;
    s_status.state = state;
    s_status.active = active;
    s_status.last_error =
        (state == XIAOZHI_FOUNDATION_SESSION_ERROR) ?
            ((error == ESP_OK) ? ESP_FAIL : error) : ESP_OK;
    generation = s_status.client_generation;
    portEXIT_CRITICAL(&s_lock);

    if ((previous != state) || (state == XIAOZHI_FOUNDATION_SESSION_ERROR)) {
        ESP_LOGI(
            TAG,
            "state %s -> %s generation=%u active=%s error=%s",
            xiaozhi_foundation_session_state_to_string(previous),
            xiaozhi_foundation_session_state_to_string(state),
            (unsigned)generation,
            active ? "yes" : "no",
            esp_err_to_name(
                (state == XIAOZHI_FOUNDATION_SESSION_ERROR) ?
                    ((error == ESP_OK) ? ESP_FAIL : error) : ESP_OK));
    }

    xiaozhi_session_publish_status();
}

static void xiaozhi_session_protocol_callback(
    esp_xiaozhi_chat_event_t event,
    void *event_data,
    void *ctx)
{
    (void)event;
    (void)event_data;
    (void)ctx;
    /* Text/audio protocol promotion belongs to later voice integration. */
}

static void xiaozhi_session_audio_callback(
    const uint8_t *data,
    int len,
    void *ctx)
{
    (void)data;
    (void)len;
    (void)ctx;
    /* Phase 13 does not route response audio through this callback yet. */
}

static void xiaozhi_session_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    EventGroupHandle_t events = NULL;
    bool active = false;
    bool intentional_stop = false;
    portENTER_CRITICAL(&s_lock);
    events = s_events;
    active = s_status.active;
    intentional_stop = s_intentional_stop;
    portEXIT_CRITICAL(&s_lock);

    if (events == NULL) {
        return;
    }

    switch (event_id) {
        case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
            (void)xEventGroupSetBits(events, XIAOZHI_SESSION_EVENT_CONNECTED);
            xiaozhi_session_set_status(
                XIAOZHI_FOUNDATION_SESSION_READY,
                true,
                ESP_OK);
            break;

        case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
            (void)xEventGroupSetBits(events, XIAOZHI_SESSION_EVENT_DISCONNECTED);
            if (intentional_stop) {
                ESP_LOGI(TAG, "DISCONNECTED observed during intentional stop");
                break;
            }
            xiaozhi_session_set_status(
                XIAOZHI_FOUNDATION_SESSION_ERROR,
                active,
                ESP_ERR_INVALID_STATE);
            break;

        case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
            (void)xEventGroupSetBits(events, XIAOZHI_SESSION_EVENT_GOODBYE);
            if (intentional_stop) {
                ESP_LOGI(TAG, "SERVER_GOODBYE observed during intentional stop");
                break;
            }
            xiaozhi_session_set_status(
                XIAOZHI_FOUNDATION_SESSION_ERROR,
                active,
                ESP_FAIL);
            break;

        default:
            break;
    }
}

static void xiaozhi_session_suppress_payload_logs(void)
{
#if CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
    if (s_payload_logs_suppressed) {
        return;
    }
    s_chat_log_level = esp_log_level_get(XIAOZHI_SESSION_UPSTREAM_CHAT_LOG_TAG);
    s_mcp_manager_log_level =
        esp_log_level_get(XIAOZHI_SESSION_UPSTREAM_MCP_MANAGER_TAG);
    s_mcp_engine_log_level =
        esp_log_level_get(XIAOZHI_SESSION_UPSTREAM_MCP_ENGINE_TAG);
    esp_log_level_set(XIAOZHI_SESSION_UPSTREAM_CHAT_LOG_TAG, ESP_LOG_NONE);
    esp_log_level_set(XIAOZHI_SESSION_UPSTREAM_MCP_MANAGER_TAG, ESP_LOG_NONE);
    esp_log_level_set(XIAOZHI_SESSION_UPSTREAM_MCP_ENGINE_TAG, ESP_LOG_NONE);
    s_payload_logs_suppressed = true;
#endif
}

static void xiaozhi_session_restore_payload_logs(void)
{
#if CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
    if (!s_payload_logs_suppressed) {
        return;
    }
    esp_log_level_set(XIAOZHI_SESSION_UPSTREAM_CHAT_LOG_TAG, s_chat_log_level);
    esp_log_level_set(
        XIAOZHI_SESSION_UPSTREAM_MCP_MANAGER_TAG,
        s_mcp_manager_log_level);
    esp_log_level_set(
        XIAOZHI_SESSION_UPSTREAM_MCP_ENGINE_TAG,
        s_mcp_engine_log_level);
    s_payload_logs_suppressed = false;
#endif
}

static esp_err_t xiaozhi_session_cleanup(void)
{
    esp_err_t first_error = ESP_OK;

    if (s_chat_started && (s_chat != 0)) {
        const esp_err_t ret = esp_xiaozhi_chat_stop(s_chat);
        if ((ret != ESP_OK) && (first_error == ESP_OK)) {
            first_error = ret;
        }
        s_chat_started = false;
    }

    if (s_event_handler_registered) {
        const esp_err_t ret = esp_event_handler_instance_unregister(
            ESP_XIAOZHI_CHAT_EVENTS,
            ESP_EVENT_ANY_ID,
            s_event_handler_instance);
        if ((ret != ESP_OK) && (first_error == ESP_OK)) {
            first_error = ret;
        }
        s_event_handler_registered = false;
        s_event_handler_instance = NULL;
    }

    if (s_chat != 0) {
        const esp_err_t ret = esp_xiaozhi_chat_deinit(s_chat);
        if ((ret != ESP_OK) && (first_error == ESP_OK)) {
            first_error = ret;
        }
        s_chat = 0;
    }

    if (s_mcp != NULL) {
        const esp_err_t ret = esp_mcp_destroy(s_mcp);
        if ((ret != ESP_OK) && (first_error == ESP_OK)) {
            first_error = ret;
        }
        s_mcp = NULL;
    }

    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }

    xiaozhi_session_restore_payload_logs();
    return first_error;
}

esp_err_t xiaozhi_foundation_session_register_status_callback(
    xiaozhi_foundation_session_status_callback_t callback,
    void *user_context)
{
    portENTER_CRITICAL(&s_lock);
    if (s_lifecycle_busy || s_status.active) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status_callback = callback;
    s_status_callback_context = user_context;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t xiaozhi_foundation_session_start(uint32_t client_generation)
{
    if (client_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_lifecycle_busy || s_status.active ||
        (s_status.state == XIAOZHI_FOUNDATION_SESSION_CONNECTING)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle_busy = true;
    s_intentional_stop = false;
    s_status.client_generation = client_generation;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_session_set_status(
        XIAOZHI_FOUNDATION_SESSION_CONNECTING,
        false,
        ESP_OK);
    xiaozhi_session_suppress_payload_logs();

    esp_err_t ret = ESP_OK;
    esp_xiaozhi_chat_info_t info = {0};
    bool info_must_be_freed = false;

    ret = esp_xiaozhi_chat_get_info(&info);
    info_must_be_freed = true;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get_info failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    if (!info.has_websocket_config) {
        ret = ESP_ERR_NOT_SUPPORTED;
        ESP_LOGE(TAG, "server did not provide WebSocket configuration");
        goto fail;
    }
    ret = esp_xiaozhi_chat_free_info(&info);
    info_must_be_freed = false;
    memset(&info, 0, sizeof(info));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "free_info failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_mcp_create(&s_mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCP create failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    esp_xiaozhi_chat_config_t chat_config = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    chat_config.mcp_engine = s_mcp;
    chat_config.owns_mcp_engine = false;
    chat_config.event_callback = xiaozhi_session_protocol_callback;
    chat_config.event_callback_ctx = NULL;
    chat_config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    chat_config.audio_callback = xiaozhi_session_audio_callback;
    chat_config.audio_callback_ctx = NULL;
    chat_config.has_mqtt_config = false;
    chat_config.has_websocket_config = true;

    ret = esp_xiaozhi_chat_init(&chat_config, &s_chat);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "chat_init failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_handler_instance_register(
        ESP_XIAOZHI_CHAT_EVENTS,
        ESP_EVENT_ANY_ID,
        xiaozhi_session_event_handler,
        NULL,
        &s_event_handler_instance);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "event handler register failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_event_handler_registered = true;

    ret = esp_xiaozhi_chat_start(s_chat);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "chat_start failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_chat_started = true;

    const EventBits_t bits = xEventGroupWaitBits(
        s_events,
        XIAOZHI_SESSION_EVENT_CONNECTED |
            XIAOZHI_SESSION_EVENT_DISCONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(XIAOZHI_SESSION_CONNECT_TIMEOUT_MS));

    if ((bits & XIAOZHI_SESSION_EVENT_DISCONNECTED) != 0U) {
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    if ((bits & XIAOZHI_SESSION_EVENT_GOODBYE) != 0U) {
        ret = ESP_FAIL;
        goto fail;
    }
    if ((bits & XIAOZHI_SESSION_EVENT_CONNECTED) == 0U) {
        ret = ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "CONNECTED timeout after %u ms",
                 (unsigned)XIAOZHI_SESSION_CONNECT_TIMEOUT_MS);
        goto fail;
    }

    portENTER_CRITICAL(&s_lock);
    s_lifecycle_busy = false;
    s_status.active = true;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_session_set_status(
        XIAOZHI_FOUNDATION_SESSION_READY,
        true,
        ESP_OK);
    ESP_LOGI(TAG, "WebSocket production session READY generation=%u",
             (unsigned)client_generation);
    return ESP_OK;

fail:
    if (info_must_be_freed) {
        const esp_err_t free_ret = esp_xiaozhi_chat_free_info(&info);
        if ((ret == ESP_OK) && (free_ret != ESP_OK)) {
            ret = free_ret;
        }
        memset(&info, 0, sizeof(info));
    }
    {
        const esp_err_t cleanup_ret = xiaozhi_session_cleanup();
        if ((ret == ESP_OK) && (cleanup_ret != ESP_OK)) {
            ret = cleanup_ret;
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_lifecycle_busy = false;
    s_intentional_stop = false;
    s_status.active = false;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_session_set_status(
        XIAOZHI_FOUNDATION_SESSION_ERROR,
        false,
        (ret == ESP_OK) ? ESP_FAIL : ret);
    return (ret == ESP_OK) ? ESP_FAIL : ret;
}

esp_err_t xiaozhi_foundation_session_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_lifecycle_busy || !s_status.active) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle_busy = true;
    s_intentional_stop = true;
    portEXIT_CRITICAL(&s_lock);

    const esp_err_t ret = xiaozhi_session_cleanup();

    portENTER_CRITICAL(&s_lock);
    s_lifecycle_busy = false;
    s_intentional_stop = false;
    s_status.active = false;
    portEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        xiaozhi_session_set_status(
            XIAOZHI_FOUNDATION_SESSION_STOPPED,
            false,
            ESP_OK);
    } else {
        xiaozhi_session_set_status(
            XIAOZHI_FOUNDATION_SESSION_ERROR,
            false,
            ret);
    }
    return ret;
}

esp_err_t xiaozhi_foundation_session_get_status(
    xiaozhi_foundation_session_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

const char *xiaozhi_foundation_session_state_to_string(
    xiaozhi_foundation_session_state_t state)
{
    switch (state) {
        case XIAOZHI_FOUNDATION_SESSION_STOPPED: return "STOPPED";
        case XIAOZHI_FOUNDATION_SESSION_CONNECTING: return "CONNECTING";
        case XIAOZHI_FOUNDATION_SESSION_READY: return "READY";
        case XIAOZHI_FOUNDATION_SESSION_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}
