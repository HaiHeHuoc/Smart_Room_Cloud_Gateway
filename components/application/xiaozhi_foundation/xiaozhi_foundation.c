/**
 * @file xiaozhi_foundation.c
 * @brief Isolated, non-sensitive Xiaozhi service and WebSocket validation.
 *
 * Phase 12.5 closes project-side MQTT support after the same pre-CONNECTED
 * failure was reproduced in both the Gateway integration and a standalone
 * official-flow esp_xiaozhi test. The foundation therefore exposes and
 * validates WebSocket only. It does not open an audio channel in this phase.
 */

/* Includes ----------------------------------------------------------------- */
#include "xiaozhi_foundation.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* Macros ------------------------------------------------------------------- */
#define XIAOZHI_FOUNDATION_PROBE_TASK_NAME \
    "xiaozhi_probe"

#define XIAOZHI_FOUNDATION_PROBE_TASK_STACK_SIZE \
    (8U * 1024U)

#define XIAOZHI_FOUNDATION_PROBE_TASK_PRIORITY \
    4U

#define XIAOZHI_FOUNDATION_EVENT_CONNECTED \
    BIT0

#define XIAOZHI_FOUNDATION_EVENT_DISCONNECTED \
    BIT1

#define XIAOZHI_FOUNDATION_CONNECT_TIMEOUT_MS \
    15000U

#define XIAOZHI_FOUNDATION_CONNECTED_HOLD_MS \
    2000U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "XIAOZHI_FOUNDATION";

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief Private state for one WebSocket validation operation.
 *
 * Only the WebSocket service capability fact and a short-lived EventGroup
 * handle are retained. No endpoint, credential, token, topic, or Xiaozhi-owned
 * string pointer is stored here.
 */
typedef struct {
    xiaozhi_foundation_transport_t requested_transport;
    bool websocket_available;
    EventGroupHandle_t events;
} xiaozhi_foundation_validation_ctx_t;

/* Static Variables --------------------------------------------------------- */
static portMUX_TYPE s_operation_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_operation_in_progress = false;

/* Function Prototypes ------------------------------------------------------ */
static void xiaozhi_foundation_transport_validation_task(
    void *argument);

static void xiaozhi_foundation_chat_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data);

static esp_err_t xiaozhi_foundation_select_transport(
    const xiaozhi_foundation_validation_ctx_t *ctx,
    xiaozhi_foundation_transport_t *selected_transport);

static const char *xiaozhi_foundation_transport_to_string(
    xiaozhi_foundation_transport_t transport);

static esp_err_t xiaozhi_foundation_validate_transport(
    xiaozhi_foundation_transport_t requested);

static esp_err_t xiaozhi_foundation_probe_impl(
    xiaozhi_foundation_info_t *out_info);

static bool xiaozhi_foundation_try_begin_operation(void);

static void xiaozhi_foundation_finish_operation(void);

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
            "Transport connection validation failed: %s",
            esp_err_to_name(ret));
    } else {
        ESP_LOGI(
            TAG,
            "Transport connection validation completed");
    }

    xiaozhi_foundation_finish_operation();
    vTaskDelete(NULL);
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

    /*
     * ESP_XIAOZHI_CHAT_EVENTS are delivered from normal ESP event-loop task
     * context, not an ISR. Keep this callback limited to recording transport
     * facts; lifecycle, logging decisions, and cleanup remain in the worker.
     */
    switch (event_id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_CONNECTED);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_DISCONNECTED);
        break;

    default:
        break;
    }
}

static bool xiaozhi_foundation_try_begin_operation(void)
{
    bool started = false;

    portENTER_CRITICAL(&s_operation_lock);

    if (!s_operation_in_progress) {
        s_operation_in_progress = true;
        started = true;
    }

    portEXIT_CRITICAL(&s_operation_lock);

    return started;
}

static void xiaozhi_foundation_finish_operation(void)
{
    portENTER_CRITICAL(&s_operation_lock);
    s_operation_in_progress = false;
    portEXIT_CRITICAL(&s_operation_lock);
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
        xiaozhi_foundation_probe_impl(&info);

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

    xiaozhi_foundation_finish_operation();

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
        .events = NULL,
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
     * The project deliberately ignores server-provided MQTT capability. Copy
     * only the WebSocket fact needed by the selected architecture before
     * releasing esp_xiaozhi-owned response storage.
     */
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
        "WebSocket available: %s",
        ctx.websocket_available ? "yes" : "no");

    xiaozhi_foundation_transport_t selected_transport;

    ret = xiaozhi_foundation_select_transport(
        &ctx,
        &selected_transport);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "No usable Xiaozhi WebSocket transport: %s",
            esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(
        TAG,
        "Transport selected: %s",
        xiaozhi_foundation_transport_to_string(
            selected_transport));

    esp_mcp_t *mcp = NULL;
    esp_xiaozhi_chat_handle_t chat = 0;
    bool chat_started = false;
    bool event_handler_registered = false;
    esp_event_handler_instance_t event_handler_instance = NULL;

    ret = esp_mcp_create(&mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create MCP engine: %s",
            esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MCP engine created");

    esp_xiaozhi_chat_config_t chat_config =
        ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();

    /*
     * Foundation owns the MCP engine for this one-shot validation. Chat borrows
     * it, therefore cleanup must deinit chat before destroying MCP.
     */
    chat_config.mcp_engine = mcp;
    chat_config.owns_mcp_engine = false;

    /*
     * Project policy is WebSocket-only. has_mqtt_config is an upstream
     * esp_xiaozhi configuration switch and is explicitly disabled here so the
     * component cannot apply its normal MQTT preference when the server
     * advertises both transports.
     */
    chat_config.has_mqtt_config = false;
    chat_config.has_websocket_config = true;

    ret = esp_xiaozhi_chat_init(
        &chat_config,
        &chat);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "chat_init failed: %s",
            esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGI(TAG, "chat_init: OK");

    ctx.events = xEventGroupCreate();
    if (ctx.events == NULL) {
        ESP_LOGE(TAG, "Failed to create transport EventGroup");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    ret = esp_event_handler_instance_register(
        ESP_XIAOZHI_CHAT_EVENTS,
        ESP_EVENT_ANY_ID,
        xiaozhi_foundation_chat_event_handler,
        &ctx,
        &event_handler_instance);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register Xiaozhi chat event handler: %s",
            esp_err_to_name(ret));
        goto cleanup;
    }

    event_handler_registered = true;

    ret = esp_xiaozhi_chat_start(chat);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "chat_start failed: %s",
            esp_err_to_name(ret));
        goto cleanup;
    }

    chat_started = true;
    ESP_LOGI(TAG, "chat_start: OK");

    const EventBits_t initial_bits =
        xEventGroupWaitBits(
            ctx.events,
            XIAOZHI_FOUNDATION_EVENT_CONNECTED |
                XIAOZHI_FOUNDATION_EVENT_DISCONNECTED,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(
                XIAOZHI_FOUNDATION_CONNECT_TIMEOUT_MS));

    /*
     * EventGroups preserve facts, not event ordering. If both connection and
     * disconnection were observed before this worker ran, fail conservatively
     * instead of declaring a stable connection.
     */
    if ((initial_bits & XIAOZHI_FOUNDATION_EVENT_DISCONNECTED) != 0U) {
        ESP_LOGE(
            TAG,
            "%s disconnected before a stable CONNECTED state",
            xiaozhi_foundation_transport_to_string(
                selected_transport));
        ret = ESP_FAIL;
        goto cleanup;
    }

    if ((initial_bits & XIAOZHI_FOUNDATION_EVENT_CONNECTED) == 0U) {
        ESP_LOGE(
            TAG,
            "Timed out waiting %u ms for %s transport connection",
            XIAOZHI_FOUNDATION_CONNECT_TIMEOUT_MS,
            xiaozhi_foundation_transport_to_string(
                selected_transport));
        ret = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    ESP_LOGI(TAG, "CONNECTED event received");
    ESP_LOGI(
        TAG,
        "Transport connected: %s",
        xiaozhi_foundation_transport_to_string(
            selected_transport));

    const EventBits_t hold_bits =
        xEventGroupWaitBits(
            ctx.events,
            XIAOZHI_FOUNDATION_EVENT_DISCONNECTED,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(
                XIAOZHI_FOUNDATION_CONNECTED_HOLD_MS));

    if ((hold_bits & XIAOZHI_FOUNDATION_EVENT_DISCONNECTED) != 0U) {
        ESP_LOGE(
            TAG,
            "%s disconnected during %u ms stability hold",
            xiaozhi_foundation_transport_to_string(
                selected_transport),
            XIAOZHI_FOUNDATION_CONNECTED_HOLD_MS);
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(
        TAG,
        "Transport stable for %u ms",
        XIAOZHI_FOUNDATION_CONNECTED_HOLD_MS);

cleanup:
    /*
     * Keep the handler context and EventGroup alive while chat_stop() runs,
     * because stop may emit DISCONNECTED. Then unregister before deleting the
     * EventGroup or returning from this stack frame.
     */
    if (chat_started) {
        const esp_err_t stop_ret =
            esp_xiaozhi_chat_stop(chat);

        if (stop_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "chat_stop failed: %s",
                esp_err_to_name(stop_ret));

            if (ret == ESP_OK) {
                ret = stop_ret;
            }
        } else {
            ESP_LOGI(TAG, "chat_stop: OK");
        }
    }

    if (event_handler_registered) {
        const esp_err_t unregister_ret =
            esp_event_handler_instance_unregister(
                ESP_XIAOZHI_CHAT_EVENTS,
                ESP_EVENT_ANY_ID,
                event_handler_instance);

        if (unregister_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to unregister Xiaozhi chat event handler: %s",
                esp_err_to_name(unregister_ret));

            if (ret == ESP_OK) {
                ret = unregister_ret;
            }
        }
    }

    if (ctx.events != NULL) {
        vEventGroupDelete(ctx.events);
        ctx.events = NULL;
    }

    if (chat != 0) {
        const esp_err_t deinit_ret =
            esp_xiaozhi_chat_deinit(chat);

        if (deinit_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "chat_deinit failed: %s",
                esp_err_to_name(deinit_ret));

            if (ret == ESP_OK) {
                ret = deinit_ret;
            }
        } else {
            ESP_LOGI(TAG, "chat_deinit: OK");
        }
    }

    if (mcp != NULL) {
        const esp_err_t destroy_ret =
            esp_mcp_destroy(mcp);

        if (destroy_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "MCP engine destroy failed: %s",
                esp_err_to_name(destroy_ret));

            if (ret == ESP_OK) {
                ret = destroy_ret;
            }
        } else {
            ESP_LOGI(TAG, "MCP engine destroyed");
        }
    }

    return ret;
}

static esp_err_t xiaozhi_foundation_probe_impl(
    xiaozhi_foundation_info_t *out_info)
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
     * Copy only scalar/non-sensitive information used by the project.
     * MQTT capability is intentionally not exposed because the selected
     * Xiaozhi architecture is WebSocket-only.
     */
    out_info->service_reachable = true;

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

/* Functions ---------------------------------------------------------------- */
esp_err_t xiaozhi_foundation_probe(xiaozhi_foundation_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!xiaozhi_foundation_try_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret =
        xiaozhi_foundation_probe_impl(out_info);

    xiaozhi_foundation_finish_operation();

    return ret;
}

esp_err_t xiaozhi_foundation_request_probe(void)
{
    if (!xiaozhi_foundation_try_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t task_created =
        xTaskCreate(
            xiaozhi_foundation_probe_task,
            XIAOZHI_FOUNDATION_PROBE_TASK_NAME,
            XIAOZHI_FOUNDATION_PROBE_TASK_STACK_SIZE,
            NULL,
            XIAOZHI_FOUNDATION_PROBE_TASK_PRIORITY,
            NULL);

    if (task_created != pdPASS) {
        xiaozhi_foundation_finish_operation();
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

    if (!xiaozhi_foundation_try_begin_operation()) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Keep service/chat work out of network/event callbacks. xTaskCreate() also
     * keeps this NVS-capable Xiaozhi lifecycle on a normal internal-RAM task
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
        xiaozhi_foundation_finish_operation();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
