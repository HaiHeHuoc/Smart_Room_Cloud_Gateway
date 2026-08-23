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

#define XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT \
    BIT2

#define XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STATE \
    BIT3

#define XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR \
    BIT4

#define XIAOZHI_FOUNDATION_EVENT_CHAT_EMOJI \
    BIT5

#define XIAOZHI_FOUNDATION_CONNECT_TIMEOUT_MS \
    15000U

#define XIAOZHI_FOUNDATION_CONNECTED_HOLD_MS \
    2000U

/* Includes the terminating NUL; copied text is never logged. */
#define XIAOZHI_FOUNDATION_CHAT_TEXT_BUFFER_SIZE \
    192U

/* Includes the terminating NUL. The component currently reports short hints. */
#define XIAOZHI_FOUNDATION_CHAT_ERROR_SOURCE_BUFFER_SIZE \
    32U

#define XIAOZHI_FOUNDATION_CHAT_EMOJI_MAX_LENGTH \
    32U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "XIAOZHI_FOUNDATION";

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief One copied text event for a single protocol role.
 *
 * The upstream text pointer is callback-lifetime only. This storage belongs to
 * the validation worker and is overwritten by a later event for the same role.
 */
typedef struct {
    bool received;
    size_t length;
    bool truncated;
    char value[XIAOZHI_FOUNDATION_CHAT_TEXT_BUFFER_SIZE];
} xiaozhi_foundation_text_snapshot_t;

/**
 * @brief Bounded, application-owned diagnostic state from protocol callbacks.
 */
typedef struct {
    xiaozhi_foundation_text_snapshot_t user_text;
    xiaozhi_foundation_text_snapshot_t assistant_text;

    bool tts_state_received;
    esp_xiaozhi_chat_tts_state_kind_t last_tts_state;

    bool error_received;
    esp_err_t last_error_code;
    bool last_error_source_truncated;
    char last_error_source[
        XIAOZHI_FOUNDATION_CHAT_ERROR_SOURCE_BUFFER_SIZE];

    bool emoji_received;
    size_t last_emoji_length;
    bool last_emoji_truncated;
} xiaozhi_foundation_protocol_state_t;

/**
 * @brief Private state for one WebSocket validation operation.
 *
 * Only the WebSocket service capability fact and a short-lived EventGroup
 * handle are retained. Protocol callback data is copied into bounded,
 * application-owned state while the worker is live; no Xiaozhi-owned string
 * pointer is stored here.
 */
typedef struct {
    xiaozhi_foundation_transport_t requested_transport;
    bool websocket_available;
    EventGroupHandle_t events;
    portMUX_TYPE protocol_lock;
    xiaozhi_foundation_protocol_state_t protocol;
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

static void xiaozhi_foundation_protocol_event_callback(
    esp_xiaozhi_chat_event_t event,
    void *event_data,
    void *ctx);

static size_t xiaozhi_foundation_copy_bounded_string(
    char *destination,
    size_t destination_size,
    const char *source,
    bool *out_truncated);

static size_t xiaozhi_foundation_get_bounded_string_length(
    const char *source,
    size_t maximum_length,
    bool *out_truncated);

static void xiaozhi_foundation_log_protocol_diagnostics(
    xiaozhi_foundation_validation_ctx_t *ctx);

static const char *xiaozhi_foundation_tts_state_to_string(
    esp_xiaozhi_chat_tts_state_kind_t state);

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

static size_t xiaozhi_foundation_copy_bounded_string(
    char *destination,
    size_t destination_size,
    const char *source,
    bool *out_truncated)
{
    size_t length = 0U;

    if (out_truncated != NULL) {
        *out_truncated = false;
    }

    if ((destination == NULL) || (destination_size == 0U)) {
        return 0U;
    }

    destination[0] = '\0';

    if (source == NULL) {
        return 0U;
    }

    while ((length < (destination_size - 1U)) &&
           (source[length] != '\0')) {
        destination[length] = source[length];
        ++length;
    }

    destination[length] = '\0';

    if (out_truncated != NULL) {
        *out_truncated = (source[length] != '\0');
    }

    return length;
}

static size_t xiaozhi_foundation_get_bounded_string_length(
    const char *source,
    size_t maximum_length,
    bool *out_truncated)
{
    size_t length = 0U;

    if (out_truncated != NULL) {
        *out_truncated = false;
    }

    if (source == NULL) {
        return 0U;
    }

    while ((length < maximum_length) && (source[length] != '\0')) {
        ++length;
    }

    if (out_truncated != NULL) {
        *out_truncated = (source[length] != '\0');
    }

    return length;
}

static void xiaozhi_foundation_protocol_event_callback(
    esp_xiaozhi_chat_event_t event,
    void *event_data,
    void *callback_ctx)
{
    xiaozhi_foundation_validation_ctx_t *ctx =
        (xiaozhi_foundation_validation_ctx_t *)callback_ctx;

    if ((ctx == NULL) || (ctx->events == NULL)) {
        return;
    }

    /*
     * The pinned component calls this directly from its protocol handlers.
     * Keep it bounded: copy only callback-lifetime data, set a fact bit, and
     * leave all logging, wait/timeout decisions, and lifecycle work to the
     * validation worker.
     */
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        const esp_xiaozhi_chat_text_data_t *const text_data =
            (const esp_xiaozhi_chat_text_data_t *)event_data;
        xiaozhi_foundation_text_snapshot_t *snapshot = NULL;
        EventBits_t event_bit = 0U;

        if ((text_data == NULL) || (text_data->text == NULL)) {
            return;
        }

        switch (text_data->role) {
        case ESP_XIAOZHI_CHAT_TEXT_ROLE_USER:
            snapshot = &ctx->protocol.user_text;
            event_bit = XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT;
            break;

        case ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT:
            snapshot = &ctx->protocol.assistant_text;
            event_bit = XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT;
            break;

        default:
            return;
        }

        /* A bounded copy is the only critical-section work in this callback. */
        portENTER_CRITICAL(&ctx->protocol_lock);
        snapshot->length = xiaozhi_foundation_copy_bounded_string(
            snapshot->value,
            sizeof(snapshot->value),
            text_data->text,
            &snapshot->truncated);
        snapshot->received = true;
        portEXIT_CRITICAL(&ctx->protocol_lock);

        (void)xEventGroupSetBits(ctx->events, event_bit);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        const esp_xiaozhi_chat_tts_state_t *const tts_state =
            (const esp_xiaozhi_chat_tts_state_t *)event_data;

        if (tts_state == NULL) {
            return;
        }

        switch (tts_state->state) {
        case ESP_XIAOZHI_CHAT_TTS_STATE_START:
        case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:
        case ESP_XIAOZHI_CHAT_TTS_STATE_STOP:
            break;

        default:
            return;
        }

        portENTER_CRITICAL(&ctx->protocol_lock);
        ctx->protocol.last_tts_state = tts_state->state;
        ctx->protocol.tts_state_received = true;
        portEXIT_CRITICAL(&ctx->protocol_lock);

        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STATE);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *const error_info =
            (const esp_xiaozhi_chat_error_info_t *)event_data;

        if (error_info == NULL) {
            return;
        }

        portENTER_CRITICAL(&ctx->protocol_lock);
        ctx->protocol.last_error_code = error_info->code;
        (void)xiaozhi_foundation_copy_bounded_string(
            ctx->protocol.last_error_source,
            sizeof(ctx->protocol.last_error_source),
            error_info->source,
            &ctx->protocol.last_error_source_truncated);
        ctx->protocol.error_received = true;
        portEXIT_CRITICAL(&ctx->protocol_lock);

        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_EMOJI: {
        const char *const emoji = (const char *)event_data;
        bool emoji_truncated = false;
        const size_t emoji_length =
            xiaozhi_foundation_get_bounded_string_length(
                emoji,
                XIAOZHI_FOUNDATION_CHAT_EMOJI_MAX_LENGTH,
                &emoji_truncated);

        if (emoji == NULL) {
            return;
        }

        portENTER_CRITICAL(&ctx->protocol_lock);
        ctx->protocol.last_emoji_length = emoji_length;
        ctx->protocol.last_emoji_truncated = emoji_truncated;
        ctx->protocol.emoji_received = true;
        portEXIT_CRITICAL(&ctx->protocol_lock);

        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_CHAT_EMOJI);
        break;
    }

    default:
        break;
    }
}

static const char *xiaozhi_foundation_tts_state_to_string(
    esp_xiaozhi_chat_tts_state_kind_t state)
{
    switch (state) {
    case ESP_XIAOZHI_CHAT_TTS_STATE_START:
        return "START";

    case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:
        return "SENTENCE_START";

    case ESP_XIAOZHI_CHAT_TTS_STATE_STOP:
        return "STOP";

    default:
        return "UNKNOWN";
    }
}

static void xiaozhi_foundation_log_protocol_diagnostics(
    xiaozhi_foundation_validation_ctx_t *ctx)
{
    if ((ctx == NULL) || (ctx->events == NULL)) {
        return;
    }

    const EventBits_t protocol_bits =
        xEventGroupGetBits(ctx->events);

    if ((protocol_bits & (XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT |
                          XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STATE |
                          XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR |
                          XIAOZHI_FOUNDATION_EVENT_CHAT_EMOJI)) == 0U) {
        return;
    }

    bool user_text_received = false;
    size_t user_text_length = 0U;
    bool user_text_truncated = false;
    bool assistant_text_received = false;
    size_t assistant_text_length = 0U;
    bool assistant_text_truncated = false;
    bool tts_state_received = false;
    esp_xiaozhi_chat_tts_state_kind_t last_tts_state =
        ESP_XIAOZHI_CHAT_TTS_STATE_START;
    bool error_received = false;
    esp_err_t last_error_code = ESP_OK;
    char last_error_source[
        XIAOZHI_FOUNDATION_CHAT_ERROR_SOURCE_BUFFER_SIZE] = {0};
    bool last_error_source_truncated = false;
    bool emoji_received = false;
    size_t last_emoji_length = 0U;
    bool last_emoji_truncated = false;

    portENTER_CRITICAL(&ctx->protocol_lock);
    user_text_received = ctx->protocol.user_text.received;
    user_text_length = ctx->protocol.user_text.length;
    user_text_truncated = ctx->protocol.user_text.truncated;
    assistant_text_received = ctx->protocol.assistant_text.received;
    assistant_text_length = ctx->protocol.assistant_text.length;
    assistant_text_truncated = ctx->protocol.assistant_text.truncated;
    tts_state_received = ctx->protocol.tts_state_received;
    last_tts_state = ctx->protocol.last_tts_state;
    error_received = ctx->protocol.error_received;
    last_error_code = ctx->protocol.last_error_code;
    last_error_source_truncated =
        ctx->protocol.last_error_source_truncated;
    (void)xiaozhi_foundation_copy_bounded_string(
        last_error_source,
        sizeof(last_error_source),
        ctx->protocol.last_error_source,
        NULL);
    emoji_received = ctx->protocol.emoji_received;
    last_emoji_length = ctx->protocol.last_emoji_length;
    last_emoji_truncated = ctx->protocol.last_emoji_truncated;
    portEXIT_CRITICAL(&ctx->protocol_lock);

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT) != 0U) &&
        user_text_received) {
        ESP_LOGI(
            TAG,
            "CHAT_TEXT role=USER len=%u%s",
            (unsigned)user_text_length,
            user_text_truncated ? " truncated" : "");
    }

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT) != 0U) &&
        assistant_text_received) {
        ESP_LOGI(
            TAG,
            "CHAT_TEXT role=ASSISTANT len=%u%s",
            (unsigned)assistant_text_length,
            assistant_text_truncated ? " truncated" : "");
    }

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STATE) != 0U) &&
        tts_state_received) {
        ESP_LOGI(
            TAG,
            "TTS state=%s",
            xiaozhi_foundation_tts_state_to_string(last_tts_state));
    }

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR) != 0U) &&
        error_received) {
        ESP_LOGW(
            TAG,
            "CHAT_ERROR code=%s source=%s%s",
            esp_err_to_name(last_error_code),
            (last_error_source[0] != '\0') ? last_error_source : "unknown",
            last_error_source_truncated ? " truncated" : "");
    }

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CHAT_EMOJI) != 0U) &&
        emoji_received) {
        ESP_LOGI(
            TAG,
            "CHAT_EMOJI len=%u%s",
            (unsigned)last_emoji_length,
            last_emoji_truncated ? " truncated" : "");
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
        .protocol_lock = portMUX_INITIALIZER_UNLOCKED,
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

    ctx.events = xEventGroupCreate();
    if (ctx.events == NULL) {
        ESP_LOGE(TAG, "Failed to create transport EventGroup");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    esp_xiaozhi_chat_config_t chat_config =
        ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();

    /*
     * Foundation owns the MCP engine for this one-shot validation. Chat borrows
     * it, therefore cleanup must deinit chat before destroying MCP.
     */
    chat_config.mcp_engine = mcp;
    chat_config.owns_mcp_engine = false;

    /*
     * Protocol callback payload pointers are borrowed by contract. The worker
     * owns ctx until chat_deinit() returns, and the callback copies only
     * bounded data into its application-owned diagnostic state.
     */
    chat_config.event_callback =
        xiaozhi_foundation_protocol_event_callback;
    chat_config.event_callback_ctx = &ctx;

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
    ESP_LOGI(TAG, "Public arbitrary text TX: NOT AVAILABLE");
    ESP_LOGI(TAG, "CHAT_TEXT receive handler: armed");

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
    ESP_LOGI(
        TAG,
        "End-to-end CHAT_TEXT validation deferred to P2-F audio/STT");

cleanup:
    /*
     * Keep the ESP event-handler context, direct protocol-callback context,
     * and EventGroup alive until chat_deinit() returns. chat_stop()/deinit()
     * can still emit connection or protocol events that refer to ctx.
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

    xiaozhi_foundation_log_protocol_diagnostics(&ctx);

    if (ctx.events != NULL) {
        vEventGroupDelete(ctx.events);
        ctx.events = NULL;
    }

    /* Do not leave copied user/assistant text on this short-lived task stack. */
    memset(&ctx.protocol, 0, sizeof(ctx.protocol));

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
