/**
 * @file xiaozhi_foundation.c
 * @brief Isolated, non-sensitive Xiaozhi service-information probe.
 *
 * This component copies only scalar availability state from the Xiaozhi
 * service response. It neither exposes Xiaozhi-owned string pointers nor
 * takes ownership of Wi-Fi or Xiaozhi lifecycle management.
 */

/* Includes ----------------------------------------------------------------- */
#include "xiaozhi_foundation.h"

#include <string.h>

#include "esp_log.h"
#include "esp_xiaozhi_info.h"

#include "esp_xiaozhi_chat.h"
#include "esp_mcp_engine.h"

#include "esp_event.h"

#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Macros ------------------------------------------------------------------- */
#define XIAOZHI_FOUNDATION_PROBE_TASK_NAME \
    "xiaozhi_probe"

#define XIAOZHI_FOUNDATION_PROBE_TASK_STACK_SIZE \
    (8U * 1024U)

#define XIAOZHI_FOUNDATION_PROBE_TASK_PRIORITY \
    4U

#define XIAOZHI_FOUNDATION_EVENT_CONNECTED      BIT0
#define XIAOZHI_FOUNDATION_EVENT_DISCONNECTED   BIT1
#define XIAOZHI_FOUNDATION_CONNECT_TIMEOUT_MS \
    15000U

#define XIAOZHI_FOUNDATION_CONNECTED_HOLD_MS \
    2000U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "XIAOZHI_FOUNDATION";

/* Type Definitions --------------------------------------------------------- */
typedef struct {
    xiaozhi_foundation_transport_t requested_transport;

    bool mqtt_available;
    bool websocket_available;

    EventGroupHandle_t events;
} xiaozhi_foundation_validation_ctx_t;

/* Static Variables --------------------------------------------------------- */
static portMUX_TYPE s_probe_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_probe_in_progress = false;

static xiaozhi_foundation_transport_t selected_transport =
    XIAOZHI_FOUNDATION_TRANSPORT_AUTO;


/* Function Prototypes ------------------------------------------------------ */
static void xiaozhi_foundation_chat_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data);

static esp_err_t xiaozhi_foundation_select_transport(
    const xiaozhi_foundation_validation_ctx_t *ctx,
    xiaozhi_foundation_transport_t *selected);

static const char *xiaozhi_foundation_transport_to_string(
    xiaozhi_foundation_transport_t transport);

static esp_err_t xiaozhi_foundation_select_transport(
    const xiaozhi_foundation_validation_ctx_t *ctx,
    xiaozhi_foundation_transport_t *selected_transport);

static void xiaozhi_foundation_transport_validation_task(
    void *argument);

static esp_err_t xiaozhi_foundation_validate_transport(
    xiaozhi_foundation_transport_t requested);
/* Static Functions --------------------------------------------------------- */
static void xiaozhi_foundation_transport_validation_task(
    void *argument)
{
    const xiaozhi_foundation_transport_t requested_transport =
        (xiaozhi_foundation_transport_t)(uintptr_t)argument;

    const esp_err_t ret =
        xiaozhi_foundation_validate_transport(
            requested_transport);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Transport validation failed: %s",
            esp_err_to_name(ret));
    } else {
        ESP_LOGI(
            TAG,
            "Transport validation completed");
    }

    vTaskDelete(NULL);
}

static esp_err_t xiaozhi_foundation_select_transport(
    const xiaozhi_foundation_validation_ctx_t *ctx,
    xiaozhi_foundation_transport_t *selected_transport)
{
    if ((ctx == NULL) || (selected_transport == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (ctx->requested_transport) {
    case XIAOZHI_FOUNDATION_TRANSPORT_AUTO:

        if (ctx->mqtt_available) {
            *selected_transport =
                XIAOZHI_FOUNDATION_TRANSPORT_MQTT;

            return ESP_OK;
        }

        if (ctx->websocket_available) {
            *selected_transport =
                XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET;

            return ESP_OK;
        }

        return ESP_ERR_NOT_FOUND;

    case XIAOZHI_FOUNDATION_TRANSPORT_MQTT:

        if (!ctx->mqtt_available) {
            return ESP_ERR_NOT_FOUND;
        }

        *selected_transport =
            XIAOZHI_FOUNDATION_TRANSPORT_MQTT;

        return ESP_OK;

    case XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET:

        if (!ctx->websocket_available) {
            return ESP_ERR_NOT_FOUND;
        }

        *selected_transport =
            XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET;

        return ESP_OK;

    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static const char *xiaozhi_foundation_transport_to_string(
    xiaozhi_foundation_transport_t transport)
{
    switch (transport) {
    case XIAOZHI_FOUNDATION_TRANSPORT_AUTO:
        return "AUTO";

    case XIAOZHI_FOUNDATION_TRANSPORT_MQTT:
        return "MQTT";

    case XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET:
        return "WebSocket";

    default:
        return "UNKNOWN";
    }
}

static void xiaozhi_foundation_chat_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)event_base;
    (void)event_data;

    xiaozhi_foundation_validation_ctx_t *ctx =
        (xiaozhi_foundation_validation_ctx_t *)arg;

    if ((ctx == NULL) || (ctx->events == NULL)) {
        return;
    }

    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_CONNECTED);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_DISCONNECTED);
        break;

    default:
        break;
    }
}

static void xiaozhi_foundation_probe_task(void *argument)
{
    (void)argument;

    xiaozhi_foundation_info_t info = {0};

    const esp_err_t ret =
        xiaozhi_foundation_probe(&info);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Background service probe failed: %s",
            esp_err_to_name(ret));
    } else {
        ESP_LOGI(
            TAG,
            "Background service probe completed");
    }

    portENTER_CRITICAL(&s_probe_lock);

    s_probe_in_progress = false;

    portEXIT_CRITICAL(&s_probe_lock);

    vTaskDelete(NULL);
}

static esp_err_t xiaozhi_foundation_validate_transport(
    xiaozhi_foundation_transport_t requested)
{
    if ((requested < XIAOZHI_FOUNDATION_TRANSPORT_AUTO) ||
        (requested > XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET)) {
        return ESP_ERR_INVALID_ARG;
    }
    else if (requested >
        XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET) {
        return ESP_ERR_INVALID_ARG;
    }
    else
    {
        ESP_LOGI(TAG,
        "Transport requested: %s",
        xiaozhi_foundation_transport_to_string(
            requested));
    }

    esp_err_t ret = ESP_OK;

    xiaozhi_foundation_validation_ctx_t ctx = {
        .requested_transport = requested,
    };


    esp_xiaozhi_chat_info_t info = {0};

    ret =
        esp_xiaozhi_chat_get_info(&info);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to get Xiaozhi service info: %s",
            esp_err_to_name(ret));

        (void)esp_xiaozhi_chat_free_info(&info);

        return ret;
    }

    ctx.mqtt_available =
        info.has_mqtt_config;

    ctx.websocket_available =
        info.has_websocket_config;

    ret = esp_xiaozhi_chat_free_info(&info);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(
        TAG,
        "MQTT available: %s",
        ctx.mqtt_available ? "yes" : "no");

    ESP_LOGI(
        TAG,
        "WebSocket available: %s",
        ctx.websocket_available ? "yes" : "no");


    xiaozhi_foundation_transport_t selected_transport;

    ret = xiaozhi_foundation_select_transport(
        &ctx,
        &selected_transport);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "No usable Xiaozhi transport: %s",
            esp_err_to_name(ret));

        return ret;
    }
    
    ESP_LOGI(
        TAG,
        "Transport selected: %s",
        xiaozhi_foundation_transport_to_string(
            selected_transport));

    return ret;
}

/* Functions ---------------------------------------------------------------- */
esp_err_t xiaozhi_foundation_probe(xiaozhi_foundation_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));

    esp_xiaozhi_chat_info_t info = {0};

    ESP_LOGI(TAG, "Probing Xiaozhi service");

    esp_err_t ret = esp_xiaozhi_chat_get_info(&info);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Xiaozhi service probe failed: %s",
                 esp_err_to_name(ret));

        /* get_info() may have partially allocated fields. */
        (void)esp_xiaozhi_chat_free_info(&info);

        return ret;
    }

    /*
     * Copy only scalar/non-sensitive information.
     * Do not expose Xiaozhi-owned string pointers.
     */
    out_info->service_reachable = true;

    out_info->mqtt_available =
        info.has_mqtt_config;

    out_info->websocket_available =
        info.has_websocket_config;

    out_info->activation_code_available =
        info.has_activation_code;

    out_info->activation_challenge_available =
        info.has_activation_challenge;

    out_info->activation_timeout_ms =
        info.activation_timeout_ms;

    out_info->server_time_available =
        info.has_server_time;

    out_info->new_firmware_available =
        info.has_new_version;

    ESP_LOGI(TAG, "Service reachable");
    ESP_LOGI(TAG, "MQTT available: %s",
             out_info->mqtt_available ? "yes" : "no");
    ESP_LOGI(TAG, "WebSocket available: %s",
             out_info->websocket_available ? "yes" : "no");
    ESP_LOGI(TAG, "Activation code: %s",
             out_info->activation_code_available ? "present" : "none");
    ESP_LOGI(TAG, "Activation challenge: %s",
             out_info->activation_challenge_available ? "present" : "none");
    ESP_LOGI(TAG, "Activation timeout: %d ms",
             out_info->activation_timeout_ms);

    ret = esp_xiaozhi_chat_free_info(&info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to free Xiaozhi info: %s",
                 esp_err_to_name(ret));

        return ret;
    }

    return ESP_OK;
}

esp_err_t xiaozhi_foundation_request_probe(void)
{
    portENTER_CRITICAL(&s_probe_lock);

    if (s_probe_in_progress) {
        portEXIT_CRITICAL(&s_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_probe_in_progress = true;

    portEXIT_CRITICAL(&s_probe_lock);

    const BaseType_t task_created =
        xTaskCreate(
            xiaozhi_foundation_probe_task,
            XIAOZHI_FOUNDATION_PROBE_TASK_NAME,
            XIAOZHI_FOUNDATION_PROBE_TASK_STACK_SIZE,
            NULL,
            XIAOZHI_FOUNDATION_PROBE_TASK_PRIORITY,
            NULL);

    if (task_created != pdPASS) {
        portENTER_CRITICAL(&s_probe_lock);

        s_probe_in_progress = false;

        portEXIT_CRITICAL(&s_probe_lock);

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t xiaozhi_foundation_request_transport_validation(
    xiaozhi_foundation_transport_t transport)
{
    if (transport >
        XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET) {
        return ESP_ERR_INVALID_ARG;
    }

    const BaseType_t task_created =
        xTaskCreate(
            xiaozhi_foundation_transport_validation_task,
            "xiaozhi_transport_test",
            8U * 1024U,
            (void *)(uintptr_t)transport,
            4U,
            NULL);

    if (task_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
