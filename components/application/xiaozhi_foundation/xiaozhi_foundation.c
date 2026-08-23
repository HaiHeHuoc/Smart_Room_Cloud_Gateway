/**
 * @file xiaozhi_foundation.c
 * @brief Isolated, non-sensitive Xiaozhi service and transport discovery.
 *
 * Phase 12.5.2 P1 stops at transport capability discovery and project-side
 * selection. It does not create a Xiaozhi chat instance, start MQTT/WebSocket,
 * open the UDP audio path, or own network lifecycle.
 */

/* Includes ----------------------------------------------------------------- */
#include "xiaozhi_foundation.h"

#include <string.h>

#include "esp_log.h"
#include "esp_xiaozhi_info.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Macros ------------------------------------------------------------------- */
#define XIAOZHI_FOUNDATION_PROBE_TASK_NAME \
    "xiaozhi_probe"

#define XIAOZHI_FOUNDATION_PROBE_TASK_STACK_SIZE \
    (8U * 1024U)

#define XIAOZHI_FOUNDATION_PROBE_TASK_PRIORITY \
    4U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "XIAOZHI_FOUNDATION";

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief Private state for one transport discovery/selection operation.
 *
 * Only scalar availability flags are copied from esp_xiaozhi. No endpoint,
 * credential, token, topic, pointer, or Xiaozhi-owned handle is retained.
 */
typedef struct {
    xiaozhi_foundation_transport_t requested_transport;
    bool mqtt_available;
    bool websocket_available;
} xiaozhi_foundation_validation_ctx_t;

/* Static Variables --------------------------------------------------------- */
static portMUX_TYPE s_probe_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_probe_in_progress = false;

/* Function Prototypes ------------------------------------------------------ */
static void xiaozhi_foundation_transport_validation_task(
    void *argument);

static esp_err_t xiaozhi_foundation_select_transport(
    const xiaozhi_foundation_validation_ctx_t *ctx,
    xiaozhi_foundation_transport_t *selected_transport);

static const char *xiaozhi_foundation_transport_to_string(
    xiaozhi_foundation_transport_t transport);

static esp_err_t xiaozhi_foundation_validate_transport(
    xiaozhi_foundation_transport_t requested);

/* Static Functions --------------------------------------------------------- */
static void xiaozhi_foundation_transport_validation_task(
    void *argument)
{
    /*
     * FreeRTOS task entry uses void *. The enum is passed as an integer-sized
     * value through uintptr_t so no temporary object or shared pointer is
     * required for this one-shot worker.
     */
    const xiaozhi_foundation_transport_t requested_transport =
        (xiaozhi_foundation_transport_t)(uintptr_t)argument;

    const esp_err_t ret =
        xiaozhi_foundation_validate_transport(
            requested_transport);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Transport selection validation failed: %s",
            esp_err_to_name(ret));
    } else {
        ESP_LOGI(
            TAG,
            "Transport selection validation completed");
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
        /*
         * Pinned esp_xiaozhi 0.1.2 prefers MQTT when both server transport
         * configurations exist. Keep AUTO aligned with that upstream policy
         * so Phase 12.5 can compare the component-preferred path first.
         */
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

    ESP_LOGI(
        TAG,
        "Transport requested: %s",
        xiaozhi_foundation_transport_to_string(requested));

    xiaozhi_foundation_validation_ctx_t ctx = {
        .requested_transport = requested,
    };

    esp_xiaozhi_chat_info_t info = {0};

    esp_err_t ret =
        esp_xiaozhi_chat_get_info(&info);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to get Xiaozhi service info: %s",
            esp_err_to_name(ret));

        /* get_info() may have partially allocated response fields. */
        (void)esp_xiaozhi_chat_free_info(&info);
        return ret;
    }

    /*
     * Copy only the capability facts needed by the project before releasing
     * esp_xiaozhi-owned response storage. The validation context remains valid
     * after free_info() because these members are plain booleans.
     */
    ctx.mqtt_available =
        info.has_mqtt_config;

    ctx.websocket_available =
        info.has_websocket_config;

    ret = esp_xiaozhi_chat_free_info(&info);
    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to free Xiaozhi service info: %s",
            esp_err_to_name(ret));
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

    /*
     * Phase 12.5.2 P1 intentionally ends here. Selection success does not mean
     * MQTT/WebSocket is connected. P2 will add MCP/chat init/start and a
     * bounded wait for the real CONNECTED event.
     */
    return ESP_OK;
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
    if ((transport < XIAOZHI_FOUNDATION_TRANSPORT_AUTO) ||
        (transport > XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET)) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Keep the service request out of network/event callbacks. xTaskCreate()
     * also keeps this NVS-capable Xiaozhi path on a normal internal-RAM task
     * stack, matching the Phase 12.4 cache-off policy.
     */
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
