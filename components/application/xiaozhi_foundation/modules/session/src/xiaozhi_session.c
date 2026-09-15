#include "xiaozhi_foundation.h"

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "app_log.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

#include "xiaozhi_mcp_cloud_push_latest.h"
#include "xiaozhi_mcp_cloud_sync.h"
#include "xiaozhi_mcp_audio_playback.h"
#include "xiaozhi_mcp_light_capabilities.h"
#include "xiaozhi_mcp_light_state_query.h"
#include "xiaozhi_mcp_light_set_state.h"
#include "xiaozhi_mcp_sensor_query.h"
#include "xiaozhi_mcp_system_status.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define XIAOZHI_SESSION_EVENT_CONNECTED            BIT0
#define XIAOZHI_SESSION_EVENT_DISCONNECTED         BIT1
#define XIAOZHI_SESSION_EVENT_GOODBYE              BIT2
#define XIAOZHI_SESSION_EVENT_AUDIO_OPENED          BIT3
#define XIAOZHI_SESSION_EVENT_AUDIO_CLOSED          BIT4
#define XIAOZHI_SESSION_EVENT_FENCE_DRAINED         BIT5
#define XIAOZHI_SESSION_CONNECT_TIMEOUT_MS         15000U
#define XIAOZHI_SESSION_AUDIO_OPEN_TIMEOUT_MS       15000U
#define XIAOZHI_SESSION_AUDIO_CLOSE_TIMEOUT_MS       8000U
#define XIAOZHI_SESSION_FENCE_DRAIN_TIMEOUT_MS       2000U
#define XIAOZHI_SESSION_FENCE_EVENT_POST_TIMEOUT_MS   100U
/* This ID is private to the project handler. It is deliberately outside the
 * small bit-style event IDs published by esp_xiaozhi_chat.h. Posting it after
 * the old WebSocket task has stopped gives a FIFO drain barrier for any old
 * global CONNECTED/DISCONNECTED/GOODBYE notifications. */
#define XIAOZHI_SESSION_PRIVATE_FENCE_DRAIN_EVENT_ID 0x7F00

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
static xiaozhi_foundation_audio_uplink_status_t s_uplink = {
    .last_error = ESP_OK,
};
static xiaozhi_foundation_session_status_callback_t s_status_callback = NULL;
static void *s_status_callback_context = NULL;
static xiaozhi_foundation_response_callback_t s_response_callback = NULL;
static void *s_response_callback_context = NULL;
static bool s_lifecycle_busy = false;
static bool s_intentional_stop = false;

typedef enum {
    XIAOZHI_SESSION_TRANSPORT_NORMAL = 0,
    XIAOZHI_SESSION_TRANSPORT_FENCE_DRAINING,
    XIAOZHI_SESSION_TRANSPORT_FENCE_STARTING,
    XIAOZHI_SESSION_TRANSPORT_FENCE_FAILED,
} xiaozhi_session_transport_phase_t;

/* Binary WebSocket callbacks carry no server turn ID. Keep delivery closed
 * while a channel is listening/closed or a transport fence is active. The
 * response owner admits only its already-reserved current epoch immediately
 * before stop-listening transmits, so the first packet cannot race its return. */
static xiaozhi_session_transport_phase_t s_transport_phase =
    XIAOZHI_SESSION_TRANSPORT_NORMAL;
static bool s_response_delivery_enabled = false;

static EventGroupHandle_t s_events = NULL;
static esp_mcp_t *s_mcp = NULL;
static esp_xiaozhi_chat_handle_t s_chat = 0;
static bool s_chat_started = false;
static bool s_event_handler_registered = false;
static esp_event_handler_instance_t s_event_handler_instance = NULL;

/* Called only while s_lock is held. */
static void xiaozhi_session_reset_uplink_locked(void);

#if CONFIG_LOG_DYNAMIC_LEVEL_CONTROL
static bool s_payload_logs_suppressed = false;
static esp_log_level_t s_chat_log_level = ESP_LOG_INFO;
static esp_log_level_t s_mcp_manager_log_level = ESP_LOG_INFO;
static esp_log_level_t s_mcp_engine_log_level = ESP_LOG_INFO;
#endif

static void xiaozhi_session_publish_status_snapshot(
    const xiaozhi_foundation_session_status_t *snapshot,
    xiaozhi_foundation_session_status_callback_t callback,
    void *callback_context)
{
    if ((snapshot != NULL) && (callback != NULL)) {
        callback(snapshot, callback_context);
    }
}

static void xiaozhi_session_publish_response(
    xiaozhi_foundation_response_event_kind_t kind,
    const uint8_t *data,
    size_t data_len,
    esp_err_t error,
    uint32_t expected_client_generation,
    bool bypass_delivery_gate)
{
    xiaozhi_foundation_response_callback_t callback = NULL;
    void *callback_context = NULL;
    uint32_t generation = 0U;
    bool delivery_allowed = false;

    portENTER_CRITICAL(&s_lock);
    callback = s_response_callback;
    callback_context = s_response_callback_context;
    generation = s_status.client_generation;
    delivery_allowed = s_response_delivery_enabled &&
        (s_transport_phase == XIAOZHI_SESSION_TRANSPORT_NORMAL) &&
        s_status.active && !s_lifecycle_busy;
    portEXIT_CRITICAL(&s_lock);

    if ((callback == NULL) ||
        ((expected_client_generation != 0U) &&
         (generation != expected_client_generation)) ||
        (!delivery_allowed && !bypass_delivery_gate)) {
        return;
    }

    const xiaozhi_foundation_response_event_t event = {
        .kind = kind,
        .client_generation = generation,
        .data = data,
        .data_len = data_len,
        .error = error,
    };
    callback(&event, callback_context);
}

static void xiaozhi_session_set_status(
    xiaozhi_foundation_session_state_t state,
    bool active,
    esp_err_t error)
{
    xiaozhi_foundation_session_state_t previous =
        XIAOZHI_FOUNDATION_SESSION_STOPPED;
    xiaozhi_foundation_session_status_t snapshot = {0};
    xiaozhi_foundation_session_status_callback_t callback = NULL;
    void *callback_context = NULL;

    portENTER_CRITICAL(&s_lock);
    previous = s_status.state;
    s_status.state = state;
    s_status.active = active;
    s_status.last_error =
        (state == XIAOZHI_FOUNDATION_SESSION_ERROR) ?
            ((error == ESP_OK) ? ESP_FAIL : error) : ESP_OK;
    snapshot = s_status;
    callback = s_status_callback;
    callback_context = s_status_callback_context;
    portEXIT_CRITICAL(&s_lock);

    if ((previous != state) || (state == XIAOZHI_FOUNDATION_SESSION_ERROR)) {
        APP_LOGI(
            TAG, STATE_S_S_GENERATION_U_B4C0AAEA,
            "state %s -> %s generation=%u active=%s error=%s",
            xiaozhi_foundation_session_state_to_string(previous),
            xiaozhi_foundation_session_state_to_string(state),
            (unsigned)snapshot.client_generation,
            active ? "yes" : "no",
            esp_err_to_name(
                (state == XIAOZHI_FOUNDATION_SESSION_ERROR) ?
                    ((error == ESP_OK) ? ESP_FAIL : error) : ESP_OK));
    }

    xiaozhi_session_publish_status_snapshot(
        &snapshot, callback, callback_context);
}

/* Global ESP_XIAOZHI_CHAT_EVENTS do not carry a transport identity. An event
 * accepted before a transport fence can still be executing when the lifecycle
 * task publishes its replacement generation. Update status only while the
 * same normal transport generation remains current, so such an old CONNECTED
 * cannot make the replacement appear READY before stop/drain/start completes. */
static bool xiaozhi_session_set_normal_status_for_generation(
    uint32_t expected_client_generation,
    xiaozhi_foundation_session_state_t state,
    bool active,
    esp_err_t error)
{
    xiaozhi_foundation_session_state_t previous =
        XIAOZHI_FOUNDATION_SESSION_STOPPED;
    xiaozhi_foundation_session_status_t snapshot = {0};
    xiaozhi_foundation_session_status_callback_t callback = NULL;
    void *callback_context = NULL;
    bool updated = false;

    portENTER_CRITICAL(&s_lock);
    if ((s_transport_phase == XIAOZHI_SESSION_TRANSPORT_NORMAL) &&
        (s_status.client_generation == expected_client_generation)) {
        previous = s_status.state;
        s_status.state = state;
        s_status.active = active;
        s_status.last_error =
            (state == XIAOZHI_FOUNDATION_SESSION_ERROR) ?
                ((error == ESP_OK) ? ESP_FAIL : error) : ESP_OK;
        snapshot = s_status;
        callback = s_status_callback;
        callback_context = s_status_callback_context;
        updated = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!updated) {
        return false;
    }

    if ((previous != state) || (state == XIAOZHI_FOUNDATION_SESSION_ERROR)) {
        APP_LOGI(
            TAG, STATE_S_S_GENERATION_U_B4C0AAEA,
            "state %s -> %s generation=%u active=%s error=%s",
            xiaozhi_foundation_session_state_to_string(previous),
            xiaozhi_foundation_session_state_to_string(state),
            (unsigned)expected_client_generation,
            active ? "yes" : "no",
            esp_err_to_name(
                (state == XIAOZHI_FOUNDATION_SESSION_ERROR) ?
                    ((error == ESP_OK) ? ESP_FAIL : error) : ESP_OK));
    }

    xiaozhi_session_publish_status_snapshot(
        &snapshot, callback, callback_context);
    return true;
}

static bool xiaozhi_session_normal_generation_is_current(
    uint32_t expected_client_generation)
{
    bool current = false;
    portENTER_CRITICAL(&s_lock);
    current = (s_transport_phase == XIAOZHI_SESSION_TRANSPORT_NORMAL) &&
        (s_status.client_generation == expected_client_generation);
    portEXIT_CRITICAL(&s_lock);
    return current;
}

static void xiaozhi_session_protocol_callback(
    esp_xiaozhi_chat_event_t event,
    void *event_data,
    void *ctx)
{
    (void)ctx;

    if (event == ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE) {
        const esp_xiaozhi_chat_tts_state_t *const tts =
            (const esp_xiaozhi_chat_tts_state_t *)event_data;
        if (tts == NULL) {
            return;
        }
        if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) {
            xiaozhi_session_publish_response(
                XIAOZHI_FOUNDATION_RESPONSE_TTS_START,
                NULL,
                0U,
                ESP_OK,
                0U,
                false);
        } else if (tts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            xiaozhi_session_publish_response(
                XIAOZHI_FOUNDATION_RESPONSE_TTS_STOP,
                NULL,
                0U,
                ESP_OK,
                0U,
                false);
        }
        return;
    }

    if (event == ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR) {
        const esp_xiaozhi_chat_error_info_t *const info =
            (const esp_xiaozhi_chat_error_info_t *)event_data;
        const esp_err_t error =
            (info != NULL && info->code != ESP_OK) ? info->code : ESP_FAIL;
        APP_LOGW(TAG, CHAT_ERROR_DELIVERED_TO_RESP_6F67E57F, "CHAT_ERROR delivered to response path: %s",
                 esp_err_to_name(error));
        xiaozhi_session_publish_response(
            XIAOZHI_FOUNDATION_RESPONSE_ERROR,
            NULL,
            0U,
            error,
            0U,
            false);
    }
}

static void xiaozhi_session_audio_callback(
    const uint8_t *data,
    int len,
    void *ctx)
{
    (void)ctx;
    if ((data == NULL) || (len <= 0)) {
        return;
    }
    xiaozhi_session_publish_response(
        XIAOZHI_FOUNDATION_RESPONSE_AUDIO,
        data,
        (size_t)len,
        ESP_OK,
        0U,
        false);
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
    bool lifecycle_busy = false;
    bool had_audio_channel = false;
    uint32_t event_generation = 0U;
    xiaozhi_session_transport_phase_t transport_phase =
        XIAOZHI_SESSION_TRANSPORT_NORMAL;
    portENTER_CRITICAL(&s_lock);
    events = s_events;
    active = s_status.active;
    intentional_stop = s_intentional_stop;
    lifecycle_busy = s_lifecycle_busy;
    had_audio_channel = s_uplink.audio_channel_open;
    transport_phase = s_transport_phase;
    event_generation = s_status.client_generation;
    portEXIT_CRITICAL(&s_lock);

    if (events == NULL) {
        return;
    }

    if (event_id == XIAOZHI_SESSION_PRIVATE_FENCE_DRAIN_EVENT_ID) {
        (void)xEventGroupSetBits(
            events, XIAOZHI_SESSION_EVENT_FENCE_DRAINED);
        return;
    }

    /* A controlled stop joins the old WebSocket producer before the private
     * marker is posted. Ignore every global event until that marker has
     * drained the old event FIFO; otherwise an old CONNECTED can make the
     * replacement client generation appear READY too early. */
    if ((transport_phase == XIAOZHI_SESSION_TRANSPORT_FENCE_DRAINING) ||
        (transport_phase == XIAOZHI_SESSION_TRANSPORT_FENCE_FAILED)) {
        return;
    }

    if (transport_phase == XIAOZHI_SESSION_TRANSPORT_FENCE_STARTING) {
        switch (event_id) {
            case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_CONNECTED);
                break;
            case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_DISCONNECTED);
                break;
            case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_GOODBYE);
                break;
            default:
                break;
        }
        return;
    }

    if (!xiaozhi_session_normal_generation_is_current(event_generation)) {
        return;
    }

    switch (event_id) {
        case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
            /* A reconnect reuses this chat/EventGroup. Clear terminal bits
             * from the old transport attempt before publishing READY. */
            (void)xEventGroupClearBits(
                events,
                XIAOZHI_SESSION_EVENT_DISCONNECTED |
                    XIAOZHI_SESSION_EVENT_GOODBYE);
            if (xiaozhi_session_set_normal_status_for_generation(
                    event_generation,
                    XIAOZHI_FOUNDATION_SESSION_READY,
                    true,
                    ESP_OK)) {
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_CONNECTED);
            }
            break;

        case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
            if (intentional_stop) {
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_DISCONNECTED);
                APP_LOGI(TAG, DISCONNECTED_OBSERVED_DURING_57F60AAB, "DISCONNECTED observed during intentional stop");
                break;
            }

            /* esp_xiaozhi keeps this WebSocket client alive and reconnects it
             * after five seconds. Do not destroy the chat instance here: a
             * teardown/recreate cycle can misattribute late global events to a
             * new generation. */
            (void)xEventGroupClearBits(
                events,
                XIAOZHI_SESSION_EVENT_CONNECTED |
                    XIAOZHI_SESSION_EVENT_AUDIO_OPENED |
                    XIAOZHI_SESSION_EVENT_AUDIO_CLOSED);
            portENTER_CRITICAL(&s_lock);
            if ((s_transport_phase != XIAOZHI_SESSION_TRANSPORT_NORMAL) ||
                (s_status.client_generation != event_generation)) {
                portEXIT_CRITICAL(&s_lock);
                break;
            }
            active = s_status.active;
            lifecycle_busy = s_lifecycle_busy;
            had_audio_channel = s_uplink.audio_channel_open;
            xiaozhi_session_reset_uplink_locked();
            portEXIT_CRITICAL(&s_lock);

            if (!active && !lifecycle_busy) {
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_DISCONNECTED);
                APP_LOGW(TAG, IGNORED_LATE_DISCONNECTED_WI_1C33ABFF, "Ignored late DISCONNECTED without an active session");
                break;
            }

            if (!xiaozhi_session_set_normal_status_for_generation(
                    event_generation,
                    XIAOZHI_FOUNDATION_SESSION_CONNECTING,
                    active,
                    ESP_OK)) {
                break;
            }
            if (had_audio_channel) {
                xiaozhi_session_publish_response(
                    XIAOZHI_FOUNDATION_RESPONSE_ERROR,
                    NULL,
                    0U,
                    ESP_ERR_INVALID_STATE,
                    event_generation,
                    true);
            }
            (void)xEventGroupSetBits(
                events, XIAOZHI_SESSION_EVENT_DISCONNECTED);
            APP_LOGW(TAG, DISCONNECTED_RETAINING_SESSI_F3AEEE05,
                     "DISCONNECTED; retaining session for upstream auto-reconnect");
            break;

        case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
            if (intentional_stop) {
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_GOODBYE);
                APP_LOGI(TAG, SERVER_GOODBYE_OBSERVED_DURI_6E2DDB1C, "SERVER_GOODBYE observed during intentional stop");
                break;
            }

            /* Xiaozhi uses goodbye to end an audio/MCP session. It does not
             * mean the WebSocket transport was lost, so it must not poison the
             * long-lived production connection after a successful turn. */
            if (!active && lifecycle_busy) {
                (void)xEventGroupSetBits(
                    events, XIAOZHI_SESSION_EVENT_GOODBYE);
                APP_LOGW(TAG, SERVER_GOODBYE_BEFORE_INITIA_83B71B47, "SERVER_GOODBYE before initial CONNECTED");
                break;
            }
            (void)xEventGroupClearBits(
                events,
                XIAOZHI_SESSION_EVENT_GOODBYE |
                    XIAOZHI_SESSION_EVENT_AUDIO_OPENED);
            portENTER_CRITICAL(&s_lock);
            if ((s_transport_phase != XIAOZHI_SESSION_TRANSPORT_NORMAL) ||
                (s_status.client_generation != event_generation)) {
                portEXIT_CRITICAL(&s_lock);
                break;
            }
            xiaozhi_session_reset_uplink_locked();
            portEXIT_CRITICAL(&s_lock);
            APP_LOGI(TAG, SERVER_GOODBYE_TREATED_AS_AU_439FC7B8,
                     "SERVER_GOODBYE treated as audio-channel completion; WebSocket retained");
            break;

        case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
            (void)xEventGroupSetBits(events, XIAOZHI_SESSION_EVENT_AUDIO_OPENED);
            break;

        case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
            portENTER_CRITICAL(&s_lock);
            if ((s_transport_phase != XIAOZHI_SESSION_TRANSPORT_NORMAL) ||
                (s_status.client_generation != event_generation)) {
                portEXIT_CRITICAL(&s_lock);
                break;
            }
            xiaozhi_session_reset_uplink_locked();
            portEXIT_CRITICAL(&s_lock);
            (void)xEventGroupSetBits(events, XIAOZHI_SESSION_EVENT_AUDIO_CLOSED);
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

static void xiaozhi_session_reset_uplink_locked(void)
{
    s_uplink = (xiaozhi_foundation_audio_uplink_status_t) {
        .client_generation = s_status.client_generation,
        .last_error = ESP_OK,
    };
    s_response_delivery_enabled = false;
}

static esp_err_t xiaozhi_session_close_uplink_best_effort(void)
{
    bool open = false;
    bool listening = false;
    portENTER_CRITICAL(&s_lock);
    open = s_uplink.audio_channel_open;
    listening = s_uplink.listening;
    /* Close the ingress gate before transmitting goodbye. A packet that was
     * already queued by the old transport is then discarded at the foundation
     * boundary instead of reaching a later local response epoch. */
    s_response_delivery_enabled = false;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t first_error = ESP_OK;
    if (listening && (s_chat != 0)) {
        const esp_err_t ret = esp_xiaozhi_chat_send_stop_listening(s_chat);
        if ((ret != ESP_OK) && (first_error == ESP_OK)) {
            first_error = ret;
        }
    }
    if (open && (s_chat != 0)) {
        if (s_events != NULL) {
            (void)xEventGroupClearBits(s_events, XIAOZHI_SESSION_EVENT_AUDIO_CLOSED);
        }
        const esp_err_t ret = esp_xiaozhi_chat_close_audio_channel(s_chat);
        if ((ret != ESP_OK) && (first_error == ESP_OK)) {
            first_error = ret;
        }
        if ((ret == ESP_OK) && (s_events != NULL)) {
            (void)xEventGroupWaitBits(
                s_events,
                XIAOZHI_SESSION_EVENT_AUDIO_CLOSED,
                pdFALSE,
                pdTRUE,
                pdMS_TO_TICKS(XIAOZHI_SESSION_AUDIO_CLOSE_TIMEOUT_MS));
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_uplink.audio_channel_open = false;
    s_uplink.listening = false;
    s_uplink.last_error = first_error;
    portEXIT_CRITICAL(&s_lock);
    return first_error;
}

static esp_err_t xiaozhi_session_cleanup(void)
{
    esp_err_t first_error = xiaozhi_session_close_uplink_best_effort();

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
        xiaozhi_mcp_audio_playback_detach();
        xiaozhi_mcp_system_status_detach();
        xiaozhi_mcp_cloud_push_latest_detach();
        xiaozhi_mcp_cloud_sync_detach();
        xiaozhi_mcp_light_capabilities_detach();
        xiaozhi_mcp_light_state_query_detach();
        xiaozhi_mcp_light_set_state_detach();
        xiaozhi_mcp_sensor_query_detach();
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

esp_err_t xiaozhi_foundation_response_register_callback(
    xiaozhi_foundation_response_callback_t callback,
    void *user_context)
{
    portENTER_CRITICAL(&s_lock);
    s_response_callback = callback;
    s_response_callback_context = user_context;
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
    s_transport_phase = XIAOZHI_SESSION_TRANSPORT_NORMAL;
    s_status.client_generation = client_generation;
    xiaozhi_session_reset_uplink_locked();
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
        APP_LOGE(TAG, GET_INFO_FAILED_S_AC4D5479, "get_info failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    if (!info.has_websocket_config) {
        ret = ESP_ERR_NOT_SUPPORTED;
        APP_LOGE(TAG, SERVER_DID_NOT_PROVIDE_WEBSO_7E20F73B, "server did not provide WebSocket configuration");
        goto fail;
    }
    ret = esp_xiaozhi_chat_free_info(&info);
    info_must_be_freed = false;
    memset(&info, 0, sizeof(info));
    if (ret != ESP_OK) {
        APP_LOGE(TAG, FREE_INFO_FAILED_S_7B1497ED, "free_info failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_mcp_create(&s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, MCP_CREATE_FAILED_S_4371E85C, "MCP create failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_sensor_query_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, SENSOR_QUERY_MCP_ATTACH_FAILED_93A649F7,
                 "Smart Room sensor MCP attach failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_cloud_sync_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, CLOUD_SYNC_MCP_ATTACH_FAILED_58D10FC7,
                 "Smart Room cloud-sync MCP attach failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_cloud_push_latest_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, CLOUD_PUSH_LATEST_MCP_ATTACH_FAIL_7A1B2CB9,
                 "Smart Room cloud push-latest MCP attach failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_system_status_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, SYSTEM_STATUS_MCP_ATTACH_FAILED_1A7C9E0D,
                 "Smart Room system-status MCP attach failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_light_set_state_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, LIGHT_SET_STATE_MCP_ATTACH_FAILED_02E039A1,
                 "Smart Room light set-state MCP attach failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_light_state_query_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, LIGHT_STATE_QUERY_MCP_ATTACH_FAILED_BB5C5A9A,
                 "Smart Room light-state MCP attach failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_light_capabilities_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, LIGHT_CAPABILITIES_MCP_ATTACH_FAILED_4DF6451F,
                 "Smart Room light capabilities MCP attach failed: %s",
                 esp_err_to_name(ret));
        goto fail;
    }

    ret = xiaozhi_mcp_audio_playback_attach(s_mcp);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, AUDIO_PLAYBACK_MCP_ATTACH_FAILED_6FB19C41,
                 "Smart Room audio playback MCP attach failed: %s",
                 esp_err_to_name(ret));
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
        APP_LOGE(TAG, CHAT_INIT_FAILED_S_6C275147, "chat_init failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = esp_event_handler_instance_register(
        ESP_XIAOZHI_CHAT_EVENTS,
        ESP_EVENT_ANY_ID,
        xiaozhi_session_event_handler,
        NULL,
        &s_event_handler_instance);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, EVENT_HANDLER_REGISTER_FAILE_FFDB2B58, "event handler register failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_event_handler_registered = true;

    (void)xEventGroupClearBits(
        s_events,
        XIAOZHI_SESSION_EVENT_CONNECTED |
            XIAOZHI_SESSION_EVENT_DISCONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE |
            XIAOZHI_SESSION_EVENT_AUDIO_OPENED |
            XIAOZHI_SESSION_EVENT_AUDIO_CLOSED);

    ret = esp_xiaozhi_chat_start(s_chat);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, CHAT_START_FAILED_S_D0A17F7D, "chat_start failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    s_chat_started = true;

    const EventBits_t bits = xEventGroupWaitBits(
        s_events,
        XIAOZHI_SESSION_EVENT_CONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(XIAOZHI_SESSION_CONNECT_TIMEOUT_MS));

    if ((bits & XIAOZHI_SESSION_EVENT_GOODBYE) != 0U) {
        ret = ESP_FAIL;
        goto fail;
    }
    if ((bits & XIAOZHI_SESSION_EVENT_CONNECTED) == 0U) {
        ret = ESP_ERR_TIMEOUT;
        APP_LOGE(TAG, CONNECTED_TIMEOUT_AFTER_U_MS_0ADAD9E9, "CONNECTED timeout after %u ms",
                 (unsigned)XIAOZHI_SESSION_CONNECT_TIMEOUT_MS);
        goto fail;
    }

    portENTER_CRITICAL(&s_lock);
    s_lifecycle_busy = false;
    s_status.active = true;
    portEXIT_CRITICAL(&s_lock);

    APP_LOGI(TAG, WEBSOCKET_PRODUCTION_SESSION_553FC0DE, "WebSocket production session CONNECTED generation=%u",
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

esp_err_t xiaozhi_foundation_audio_uplink_start(uint32_t client_generation)
{
    if (client_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    EventGroupHandle_t events = NULL;
    portENTER_CRITICAL(&s_lock);
    const bool valid =
        !s_lifecycle_busy && s_status.active &&
        (s_status.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
        (s_status.client_generation == client_generation) &&
        (s_transport_phase == XIAOZHI_SESSION_TRANSPORT_NORMAL) &&
        !s_uplink.audio_channel_open && !s_uplink.listening &&
        (s_chat != 0) && (s_events != NULL);
    if (valid) {
        /* Do not admit a previous turn's raw packet while this new channel is
         * handshaking/listening. It becomes eligible only after this turn's
         * downlink response epoch is reserved by the dedicated stop path. */
        s_response_delivery_enabled = false;
    }
    events = s_events;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }

    /* The EventGroup lives for the long-lived WebSocket session. Each audio
     * channel open must start with fresh evidence, not stale disconnect or
     * goodbye bits from a previous turn. */
    (void)xEventGroupClearBits(
        events,
        XIAOZHI_SESSION_EVENT_AUDIO_OPENED |
            XIAOZHI_SESSION_EVENT_AUDIO_CLOSED |
            XIAOZHI_SESSION_EVENT_DISCONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE);
    esp_xiaozhi_chat_audio_t audio = {
        .format = "opus",
        .sample_rate = XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ,
        .channels = XIAOZHI_FOUNDATION_UPLINK_CHANNELS,
        .frame_duration = XIAOZHI_FOUNDATION_OPUS_FRAME_DURATION_MS,
    };
    esp_err_t ret = esp_xiaozhi_chat_open_audio_channel(
        s_chat, &audio, NULL, 0U);
    if (ret != ESP_OK) {
        goto fail;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        events,
        XIAOZHI_SESSION_EVENT_AUDIO_OPENED |
            XIAOZHI_SESSION_EVENT_DISCONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(XIAOZHI_SESSION_AUDIO_OPEN_TIMEOUT_MS));
    if ((bits & XIAOZHI_SESSION_EVENT_AUDIO_OPENED) == 0U) {
        ret = ((bits & (XIAOZHI_SESSION_EVENT_DISCONNECTED |
                        XIAOZHI_SESSION_EVENT_GOODBYE)) != 0U)
                  ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        goto fail;
    }

    ret = esp_xiaozhi_chat_send_start_listening(
        s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
    if (ret != ESP_OK) {
        (void)esp_xiaozhi_chat_close_audio_channel(s_chat);
        goto fail;
    }

    portENTER_CRITICAL(&s_lock);
    s_uplink.audio_channel_open = true;
    s_uplink.listening = true;
    s_uplink.client_generation = client_generation;
    s_uplink.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    APP_LOGI(TAG, AUDIO_CHANNEL_READY_GENERATI_7B54A6E2, "audio channel READY generation=%u format=opus rate=%u channels=1 frame_ms=%u",
             (unsigned)client_generation,
             (unsigned)XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ,
             (unsigned)XIAOZHI_FOUNDATION_OPUS_FRAME_DURATION_MS);
    return ESP_OK;

fail:
    portENTER_CRITICAL(&s_lock);
    s_uplink.audio_channel_open = false;
    s_uplink.listening = false;
    s_uplink.client_generation = client_generation;
    s_uplink.last_error = (ret == ESP_OK) ? ESP_FAIL : ret;
    portEXIT_CRITICAL(&s_lock);
    return (ret == ESP_OK) ? ESP_FAIL : ret;
}

esp_err_t xiaozhi_foundation_audio_uplink_send_opus_packet(
    uint32_t client_generation,
    const uint8_t *packet,
    size_t packet_size)
{
    if ((client_generation == 0U) || (packet == NULL) ||
        (packet_size == 0U) ||
        (packet_size > XIAOZHI_FOUNDATION_OPUS_MAX_PACKET_BYTES)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    const bool valid =
        s_status.active &&
        (s_status.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
        (s_status.client_generation == client_generation) &&
        s_uplink.audio_channel_open && s_uplink.listening &&
        (s_uplink.client_generation == client_generation) &&
        (s_chat != 0);
    const esp_xiaozhi_chat_handle_t chat = s_chat;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = esp_xiaozhi_chat_send_audio_data(
        chat, (const char *)packet, packet_size);

    portENTER_CRITICAL(&s_lock);
    if ((s_uplink.client_generation == client_generation) &&
        s_uplink.audio_channel_open) {
        if (ret == ESP_OK) {
            ++s_uplink.frames_sent;
            s_uplink.samples_sent += XIAOZHI_FOUNDATION_OPUS_FRAME_SAMPLES;
            s_uplink.bytes_sent += packet_size;
            s_uplink.last_error = ESP_OK;
        } else {
            s_uplink.last_error = ret;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

static esp_err_t xiaozhi_session_audio_uplink_stop(
    uint32_t client_generation,
    bool admit_reserved_response)
{
    if (client_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_xiaozhi_chat_handle_t chat = 0;
    portENTER_CRITICAL(&s_lock);
    const bool valid =
        !s_lifecycle_busy && s_status.active &&
        (s_status.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
        (s_status.client_generation == client_generation) &&
        (s_transport_phase == XIAOZHI_SESSION_TRANSPORT_NORMAL) &&
        (s_uplink.client_generation == client_generation) &&
        s_uplink.audio_channel_open && s_uplink.listening &&
        (s_chat != 0);
    chat = s_chat;
    /* uplink_end_turn reserved the callback epoch before this point. Open the
     * gate before the transmit only for that current response, otherwise a
     * fast TTS_START/first Opus packet can arrive before send returns. */
    s_response_delivery_enabled = valid && admit_reserved_response;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = esp_xiaozhi_chat_send_stop_listening(chat);
    portENTER_CRITICAL(&s_lock);
    if (s_uplink.client_generation == client_generation) {
        if (ret == ESP_OK) {
            s_uplink.listening = false;
        } else if (!admit_reserved_response) {
            s_response_delivery_enabled = false;
        }
        s_uplink.last_error = ret;
    }
    portEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        APP_LOGI(TAG, LISTENING_STOP_GENERATION_U_483FF50F,
                 "listening STOP generation=%u; response channel retained admission=%s",
                 (unsigned)client_generation,
                 admit_reserved_response ? "yes" : "no");
    }
    return ret;
}

esp_err_t xiaozhi_foundation_audio_uplink_stop(uint32_t client_generation)
{
    return xiaozhi_session_audio_uplink_stop(client_generation, false);
}

esp_err_t xiaozhi_foundation_audio_uplink_stop_for_response(
    uint32_t client_generation)
{
    return xiaozhi_session_audio_uplink_stop(client_generation, true);
}

esp_err_t xiaozhi_foundation_audio_channel_close(uint32_t client_generation)
{
    if (client_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    const bool valid =
        (s_status.client_generation == client_generation) &&
        (s_uplink.client_generation == client_generation) &&
        s_uplink.audio_channel_open;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }
    return xiaozhi_session_close_uplink_best_effort();
}

esp_err_t xiaozhi_foundation_audio_abort_response(
    uint32_t client_generation)
{
    if (client_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_xiaozhi_chat_handle_t chat = 0;
    portENTER_CRITICAL(&s_lock);
    const bool valid =
        !s_lifecycle_busy && s_status.active &&
        (s_status.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
        (s_status.client_generation == client_generation) &&
        (s_transport_phase == XIAOZHI_SESSION_TRANSPORT_NORMAL) &&
        (s_uplink.client_generation == client_generation) &&
        s_uplink.audio_channel_open && (s_chat != 0);
    chat = s_chat;
    /* Gate first: the upstream binary callback has no session/turn metadata,
     * so already-buffered old Opus frames must not reach a future response. */
    if (valid) {
        s_response_delivery_enabled = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = esp_xiaozhi_chat_send_abort_speaking(
        chat, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_WAKE_WORD_DETECTED);
    portENTER_CRITICAL(&s_lock);
    if ((s_status.client_generation == client_generation) &&
        (s_uplink.client_generation == client_generation) &&
        s_uplink.audio_channel_open) {
        s_uplink.last_error = ret;
    }
    portEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        APP_LOGI(TAG, RESPONSE_ABORT_SIGNALLED_GENERAT_5CF49BE6,
                 "response abort signalled generation=%u before channel close",
                 (unsigned)client_generation);
    }
    return ret;
}

esp_err_t xiaozhi_foundation_session_rotate_transport(
    uint32_t expected_client_generation,
    uint32_t replacement_client_generation)
{
    if ((expected_client_generation == 0U) ||
        (replacement_client_generation == 0U) ||
        (expected_client_generation == replacement_client_generation)) {
        return ESP_ERR_INVALID_ARG;
    }

    EventGroupHandle_t events = NULL;
    esp_xiaozhi_chat_handle_t chat = 0;
    xiaozhi_foundation_session_status_t status_snapshot = {0};
    xiaozhi_foundation_session_status_callback_t status_callback = NULL;
    void *status_callback_context = NULL;
    portENTER_CRITICAL(&s_lock);
    const bool valid =
        !s_lifecycle_busy && s_status.active &&
        (s_status.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
        (s_status.client_generation == expected_client_generation) &&
        (s_chat != 0) && s_chat_started && (s_events != NULL) &&
        (s_transport_phase == XIAOZHI_SESSION_TRANSPORT_NORMAL);
    if (valid) {
        /* The caller has already reserved replacement_client_generation in
         * voice_assistant. Commit it here before stopping transport so every
         * late old status is rejected at the product boundary as well. */
        s_lifecycle_busy = true;
        s_intentional_stop = true;
        s_transport_phase = XIAOZHI_SESSION_TRANSPORT_FENCE_DRAINING;
        s_status.client_generation = replacement_client_generation;
        s_status.state = XIAOZHI_FOUNDATION_SESSION_CONNECTING;
        s_status.active = true;
        s_status.last_error = ESP_OK;
        xiaozhi_session_reset_uplink_locked();
        status_snapshot = s_status;
        status_callback = s_status_callback;
        status_callback_context = s_status_callback_context;
        events = s_events;
        chat = s_chat;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }

    xiaozhi_session_publish_status_snapshot(
        &status_snapshot, status_callback, status_callback_context);
    APP_LOGI(TAG, TRANSPORT_FENCE_BEGIN_OLD_U_NEW_8068A3F9,
             "transport fence begin old_generation=%u replacement_generation=%u",
             (unsigned)expected_client_generation,
             (unsigned)replacement_client_generation);

    esp_err_t ret = esp_xiaozhi_chat_stop(chat);
    if (ret != ESP_OK) {
        goto fence_failed;
    }
    portENTER_CRITICAL(&s_lock);
    s_chat_started = false;
    s_intentional_stop = false;
    portEXIT_CRITICAL(&s_lock);

    (void)xEventGroupClearBits(
        events,
        XIAOZHI_SESSION_EVENT_CONNECTED |
            XIAOZHI_SESSION_EVENT_DISCONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE |
            XIAOZHI_SESSION_EVENT_AUDIO_OPENED |
            XIAOZHI_SESSION_EVENT_AUDIO_CLOSED |
            XIAOZHI_SESSION_EVENT_FENCE_DRAINED);
    ret = esp_event_post(
        ESP_XIAOZHI_CHAT_EVENTS,
        XIAOZHI_SESSION_PRIVATE_FENCE_DRAIN_EVENT_ID,
        NULL,
        0U,
        pdMS_TO_TICKS(XIAOZHI_SESSION_FENCE_EVENT_POST_TIMEOUT_MS));
    if (ret != ESP_OK) {
        goto fence_failed;
    }
    const EventBits_t drain_bits = xEventGroupWaitBits(
        events,
        XIAOZHI_SESSION_EVENT_FENCE_DRAINED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(XIAOZHI_SESSION_FENCE_DRAIN_TIMEOUT_MS));
    if ((drain_bits & XIAOZHI_SESSION_EVENT_FENCE_DRAINED) == 0U) {
        ret = ESP_ERR_TIMEOUT;
        goto fence_failed;
    }

    (void)xEventGroupClearBits(
        events,
        XIAOZHI_SESSION_EVENT_CONNECTED |
            XIAOZHI_SESSION_EVENT_DISCONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE |
            XIAOZHI_SESSION_EVENT_AUDIO_OPENED |
            XIAOZHI_SESSION_EVENT_AUDIO_CLOSED);
    portENTER_CRITICAL(&s_lock);
    s_transport_phase = XIAOZHI_SESSION_TRANSPORT_FENCE_STARTING;
    portEXIT_CRITICAL(&s_lock);

    ret = esp_xiaozhi_chat_start(chat);
    if (ret != ESP_OK) {
        goto fence_failed;
    }
    portENTER_CRITICAL(&s_lock);
    s_chat_started = true;
    portEXIT_CRITICAL(&s_lock);

    const EventBits_t connected_bits = xEventGroupWaitBits(
        events,
        XIAOZHI_SESSION_EVENT_CONNECTED |
            XIAOZHI_SESSION_EVENT_DISCONNECTED |
            XIAOZHI_SESSION_EVENT_GOODBYE,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(XIAOZHI_SESSION_CONNECT_TIMEOUT_MS));
    if (((connected_bits & XIAOZHI_SESSION_EVENT_CONNECTED) == 0U) ||
        ((connected_bits & (XIAOZHI_SESSION_EVENT_DISCONNECTED |
                            XIAOZHI_SESSION_EVENT_GOODBYE)) != 0U)) {
        ret = ((connected_bits & (XIAOZHI_SESSION_EVENT_DISCONNECTED |
                                  XIAOZHI_SESSION_EVENT_GOODBYE)) != 0U)
                  ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        goto fence_failed;
    }

    portENTER_CRITICAL(&s_lock);
    s_lifecycle_busy = false;
    s_intentional_stop = false;
    s_transport_phase = XIAOZHI_SESSION_TRANSPORT_NORMAL;
    /* A fresh connection is not a response admission. A later response-owned
     * stop-listening path opens this ingress gate immediately before send. */
    s_response_delivery_enabled = false;
    s_status.state = XIAOZHI_FOUNDATION_SESSION_READY;
    s_status.active = true;
    s_status.last_error = ESP_OK;
    status_snapshot = s_status;
    status_callback = s_status_callback;
    status_callback_context = s_status_callback_context;
    portEXIT_CRITICAL(&s_lock);
    xiaozhi_session_publish_status_snapshot(
        &status_snapshot, status_callback, status_callback_context);
    APP_LOGI(TAG, TRANSPORT_FENCE_READY_GENERATION_9B1D629D,
             "transport fence READY generation=%u",
             (unsigned)replacement_client_generation);
    return ESP_OK;

fence_failed:
    /* A fresh transport is mandatory after a local abort. Do not reopen the
     * response gate on any stop/start/barrier failure. Best-effort stop is
     * safe here and leaves the retained chat/MCP objects for normal recovery
     * cleanup rather than recreating them from this path. */
    (void)esp_xiaozhi_chat_stop(chat);
    ret = (ret == ESP_OK) ? ESP_FAIL : ret;
    portENTER_CRITICAL(&s_lock);
    s_chat_started = false;
    s_lifecycle_busy = false;
    s_intentional_stop = false;
    s_transport_phase = XIAOZHI_SESSION_TRANSPORT_FENCE_FAILED;
    xiaozhi_session_reset_uplink_locked();
    s_status.state = XIAOZHI_FOUNDATION_SESSION_ERROR;
    s_status.active = true;
    s_status.last_error = ret;
    status_snapshot = s_status;
    status_callback = s_status_callback;
    status_callback_context = s_status_callback_context;
    portEXIT_CRITICAL(&s_lock);
    xiaozhi_session_publish_status_snapshot(
        &status_snapshot, status_callback, status_callback_context);
    APP_LOGE(TAG, TRANSPORT_FENCE_FAILED_GENERATI_1B6DBB6F,
             "transport fence failed replacement_generation=%u error=%s",
             (unsigned)replacement_client_generation,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t xiaozhi_foundation_audio_uplink_get_status(
    xiaozhi_foundation_audio_uplink_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_uplink;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
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
    xiaozhi_session_reset_uplink_locked();
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
