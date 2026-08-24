/**
 * @file xiaozhi_foundation.c
 * @brief Isolated, non-sensitive Xiaozhi service and WebSocket validation.
 *
 * Phase 12.5 closes project-side MQTT support after the same pre-CONNECTED
 * failure was reproduced in both the Gateway integration and a standalone
 * official-flow esp_xiaozhi test. The foundation therefore exposes and
 * validates WebSocket only. P2-E validates the WebSocket audio-channel
 * lifecycle; an explicitly enabled P2-F test can stream one embedded,
 * validation-only Opus fixture. Neither path owns production audio hardware.
 */

/* Includes ----------------------------------------------------------------- */
#include "xiaozhi_foundation.h"

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_timer.h"
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

#define XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_OPENED \
    BIT6

#define XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_CLOSED \
    BIT7

#define XIAOZHI_FOUNDATION_EVENT_AUDIO_DATA_INCOMING \
    BIT8

#define XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_USER \
    BIT9

#define XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_ASSISTANT \
    BIT10

#define XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_START \
    BIT11

#define XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_SENTENCE_START \
    BIT12

#define XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STOP \
    BIT13

#define XIAOZHI_FOUNDATION_EVENT_AUDIO_RX \
    BIT14

#define XIAOZHI_FOUNDATION_EVENT_CONVERSATION_TURN_COMPLETE \
    BIT15

#define XIAOZHI_FOUNDATION_CONNECT_TIMEOUT_MS \
    15000U

#define XIAOZHI_FOUNDATION_AUDIO_CHANNEL_OPEN_TIMEOUT_MS \
    15000U

#define XIAOZHI_FOUNDATION_AUDIO_CHANNEL_CLOSE_TIMEOUT_MS \
    8000U

#define XIAOZHI_FOUNDATION_AUDIO_CHANNEL_HOLD_MS \
    1000U

#define XIAOZHI_FOUNDATION_P2F_RESPONSE_TIMEOUT_MS \
    30000U

#define XIAOZHI_FOUNDATION_AUDIO_FORMAT \
    "opus"

#define XIAOZHI_FOUNDATION_AUDIO_SAMPLE_RATE \
    16000

#define XIAOZHI_FOUNDATION_AUDIO_CHANNELS \
    1

#define XIAOZHI_FOUNDATION_AUDIO_FRAME_DURATION_MS \
    60

/* Includes the terminating NUL. P2-F logs a separately bounded safe copy. */
#define XIAOZHI_FOUNDATION_CHAT_TEXT_BUFFER_SIZE \
    192U

_Static_assert(
    XIAOZHI_FOUNDATION_CHAT_TEXT_BUFFER_SIZE ==
        XIAOZHI_FOUNDATION_UI_TEXT_BUFFER_SIZE,
    "validation UI text buffer must match the copied protocol text bound");

/* Includes the terminating NUL. The component currently reports short hints. */
#define XIAOZHI_FOUNDATION_CHAT_ERROR_SOURCE_BUFFER_SIZE \
    32U

#define XIAOZHI_FOUNDATION_CHAT_EMOJI_MAX_LENGTH \
    32U

/* P2-F fixture is a test-only XZF1 raw-Opus packet stream. */
#define XIAOZHI_FOUNDATION_P2F_FIXTURE_MAGIC \
    "XZF1"

#define XIAOZHI_FOUNDATION_P2F_FIXTURE_VERSION \
    1U

#define XIAOZHI_FOUNDATION_P2F_FIXTURE_CODEC_OPUS \
    1U

#define XIAOZHI_FOUNDATION_P2F_FIXTURE_HEADER_SIZE \
    24U

#define XIAOZHI_FOUNDATION_P2F_MAX_FIXTURE_BYTES \
    (64U * 1024U)

#define XIAOZHI_FOUNDATION_P2F_MAX_FRAMES \
    120U

#define XIAOZHI_FOUNDATION_P2F_MAX_FRAME_BYTES \
    2048U

/* Includes the terminating NUL and excludes untrusted control characters. */
#define XIAOZHI_FOUNDATION_P2F_LOG_TEXT_BUFFER_SIZE \
    97U

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
    bool conversation_turn_complete;

    bool tts_state_received;
    esp_xiaozhi_chat_tts_state_kind_t last_tts_state;
    bool tts_start_received;
    bool tts_sentence_start_received;
    bool tts_stop_received;

    size_t audio_rx_first_packet_size;
    size_t audio_rx_max_packet_size;

    bool first_error_received;
    esp_err_t first_error_code;
    bool first_error_source_truncated;
    char first_error_source[
        XIAOZHI_FOUNDATION_CHAT_ERROR_SOURCE_BUFFER_SIZE];

    bool emoji_received;
    size_t last_emoji_length;
    bool last_emoji_truncated;
} xiaozhi_foundation_protocol_state_t;

/**
 * @brief Saturating facts collected only for one validation transaction.
 *
 * Callbacks update these fields under protocol_lock. They are scalar evidence,
 * not a continuous performance monitor or a source of production telemetry.
 */
typedef struct {
    uint32_t validation_attempt_count;
    uint32_t validation_pass_count;
    uint32_t validation_fail_count;
    uint32_t connected_event_count;
    uint32_t disconnected_event_count;
    uint32_t audio_channel_opened_count;
    uint32_t audio_channel_closed_count;
    uint32_t chat_error_count;
    uint32_t user_text_event_count;
    uint32_t assistant_text_event_count;
    uint32_t conversation_turn_complete_count;
    uint32_t tts_start_count;
    uint32_t tts_sentence_start_count;
    uint32_t tts_stop_count;
    uint32_t audio_rx_callback_count;
    uint64_t audio_rx_total_bytes;
} xiaozhi_foundation_validation_counters_t;

/**
 * @brief First operation error and first independent cleanup error.
 *
 * Cleanup cannot overwrite a meaningful lifecycle or protocol failure. Both
 * facts are retained for the worker's final deterministic summary.
 */
typedef struct {
    bool primary_error_recorded;
    esp_err_t primary_error;
    bool cleanup_error_recorded;
    esp_err_t cleanup_error;
} xiaozhi_foundation_validation_result_t;

/**
 * @brief One synchronous heap/stack observation made by the validation worker.
 *
 * DMA-capable memory overlaps some internal memory, so these capability pools
 * are intentionally reported independently and are never summed.
 */
typedef struct {
    size_t internal_free_bytes;
    size_t internal_min_free_bytes;
    size_t internal_largest_block_bytes;
    size_t dma_free_bytes;
    size_t dma_min_free_bytes;
    size_t dma_largest_block_bytes;
    size_t psram_free_bytes;
    size_t psram_min_free_bytes;
    size_t psram_largest_block_bytes;
    UBaseType_t worker_stack_high_water_words;
} xiaozhi_foundation_resource_snapshot_t;

typedef enum {
    XIAOZHI_FOUNDATION_VALIDATION_P2E_AUDIO_CHANNEL = 0,
    XIAOZHI_FOUNDATION_VALIDATION_P2F_AUDIO_E2E,
} xiaozhi_foundation_validation_checkpoint_t;

/**
 * @brief Parsed, application-owned view of a validation-only fixture.
 *
 * The raw packet bytes remain in immutable embedded storage. Each record is
 * one complete Opus packet and is never retained by a callback.
 */
typedef struct {
    const uint8_t *first_frame;
    const uint8_t *end;
    uint32_t frame_count;
    uint16_t frame_duration_ms;
} xiaozhi_foundation_p2f_fixture_t;

/**
 * @brief State retained by the one-shot validation worker for UI observation.
 *
 * This is a copied, non-production presentation model. The foundation still
 * owns all protocol and lifecycle decisions; a registered observer may only
 * receive snapshots through the application composition layer.
 */
typedef struct {
    xiaozhi_foundation_ui_state_t state;
    int64_t listening_started_at_us;
    int64_t listening_stopped_at_us;
    esp_err_t last_error;
} xiaozhi_foundation_ui_model_t;

/**
 * @brief Private state for one WebSocket validation operation.
 *
 * Only the WebSocket service capability fact and a short-lived EventGroup
 * handle are retained. Protocol callback data is copied into bounded,
 * application-owned state while the worker is live; no Xiaozhi-owned string
 * pointer is stored here. protocol_lock protects protocol, counters, and
 * result facts as one bounded callback-owned diagnostic domain.
 */
typedef struct {
    xiaozhi_foundation_transport_t requested_transport;
    bool websocket_available;
    EventGroupHandle_t events;
    portMUX_TYPE protocol_lock;
    xiaozhi_foundation_protocol_state_t protocol;
    xiaozhi_foundation_validation_counters_t counters;
    xiaozhi_foundation_validation_result_t result;
    xiaozhi_foundation_ui_model_t ui;
} xiaozhi_foundation_validation_ctx_t;

/* Static Variables --------------------------------------------------------- */
static portMUX_TYPE s_operation_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_operation_in_progress = false;

/* Application composition owns the callback/context lifetime. */
static xiaozhi_foundation_ui_status_callback_t s_ui_status_callback = NULL;
static void *s_ui_status_callback_context = NULL;

#if CONFIG_XIAOZHI_FOUNDATION_P2F_EMBED_FIXTURE
extern const uint8_t xiaozhi_p2f_fixture_start[]
    asm("_binary_xiaozhi_p2f_fixture_start");
extern const uint8_t xiaozhi_p2f_fixture_end[]
    asm("_binary_xiaozhi_p2f_fixture_end");
#endif

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

static void xiaozhi_foundation_audio_callback(
    const uint8_t *data,
    int len,
    void *callback_ctx);

static void xiaozhi_foundation_publish_ui_status(
    xiaozhi_foundation_validation_ctx_t *ctx);

static void xiaozhi_foundation_set_ui_state(
    xiaozhi_foundation_validation_ctx_t *ctx,
    xiaozhi_foundation_ui_state_t state,
    esp_err_t last_error);

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

static void xiaozhi_foundation_log_p2f_evidence(
    xiaozhi_foundation_validation_ctx_t *ctx);

static void xiaozhi_foundation_capture_resource_snapshot(
    xiaozhi_foundation_resource_snapshot_t *snapshot,
    const char *checkpoint);

static esp_err_t xiaozhi_foundation_log_validation_summary(
    xiaozhi_foundation_validation_ctx_t *ctx,
    const char *checkpoint_name,
    const xiaozhi_foundation_resource_snapshot_t *before,
    const xiaozhi_foundation_resource_snapshot_t *after);

static void xiaozhi_foundation_record_primary_error(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_err_t error);

static void xiaozhi_foundation_record_cleanup_error(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_err_t error);

static esp_err_t xiaozhi_foundation_validate_p2e_audio_channel(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open);

static esp_err_t xiaozhi_foundation_validate_p2f_audio_e2e(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open);

static esp_err_t xiaozhi_foundation_open_audio_channel(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open);

static esp_err_t xiaozhi_foundation_close_audio_channel(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open,
    bool cleanup);

static esp_err_t xiaozhi_foundation_wait_for_event(
    xiaozhi_foundation_validation_ctx_t *ctx,
    EventBits_t expected,
    uint32_t timeout_ms,
    const char *operation);

static esp_err_t xiaozhi_foundation_get_runtime_error(
    xiaozhi_foundation_validation_ctx_t *ctx,
    const char *operation);

static esp_err_t xiaozhi_foundation_parse_p2f_fixture(
    const uint8_t *data,
    size_t data_size,
    xiaozhi_foundation_p2f_fixture_t *out_fixture);

static esp_err_t xiaozhi_foundation_get_p2f_fixture(
    const uint8_t **out_data,
    size_t *out_size);

static esp_err_t xiaozhi_foundation_send_p2f_fixture(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    const xiaozhi_foundation_p2f_fixture_t *fixture,
    uint32_t *out_frames_sent,
    size_t *out_bytes_sent);

static esp_err_t xiaozhi_foundation_wait_for_p2f_evidence(
    xiaozhi_foundation_validation_ctx_t *ctx);

static xiaozhi_foundation_validation_checkpoint_t
xiaozhi_foundation_get_validation_checkpoint(void);

static const char *xiaozhi_foundation_validation_checkpoint_to_string(
    xiaozhi_foundation_validation_checkpoint_t checkpoint);

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
        ESP_LOGE(TAG, "Xiaozhi validation worker finished: %s",
                 esp_err_to_name(ret));
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
        portENTER_CRITICAL(&ctx->protocol_lock);
        if (ctx->counters.connected_event_count < UINT32_MAX) {
            ++ctx->counters.connected_event_count;
        }
        portEXIT_CRITICAL(&ctx->protocol_lock);
        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_CONNECTED);
        xiaozhi_foundation_set_ui_state(
            ctx,
            XIAOZHI_FOUNDATION_UI_READY,
            ESP_OK);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        portENTER_CRITICAL(&ctx->protocol_lock);
        if (ctx->counters.disconnected_event_count < UINT32_MAX) {
            ++ctx->counters.disconnected_event_count;
        }
        portEXIT_CRITICAL(&ctx->protocol_lock);
        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_DISCONNECTED);
        xiaozhi_foundation_set_ui_state(
            ctx,
            XIAOZHI_FOUNDATION_UI_DISCONNECTED,
            ESP_OK);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        portENTER_CRITICAL(&ctx->protocol_lock);
        if (ctx->counters.audio_channel_opened_count < UINT32_MAX) {
            ++ctx->counters.audio_channel_opened_count;
        }
        portEXIT_CRITICAL(&ctx->protocol_lock);
        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_OPENED);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        portENTER_CRITICAL(&ctx->protocol_lock);
        if (ctx->counters.audio_channel_closed_count < UINT32_MAX) {
            ++ctx->counters.audio_channel_closed_count;
        }
        portEXIT_CRITICAL(&ctx->protocol_lock);
        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_CLOSED);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_DATA_INCOMING:
        /* 0.1.2 exposes this ID but does not post it; RX uses audio_callback. */
        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_AUDIO_DATA_INCOMING);
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

static void xiaozhi_foundation_publish_ui_status(
    xiaozhi_foundation_validation_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    xiaozhi_foundation_ui_status_callback_t callback = NULL;
    void *callback_context = NULL;
    xiaozhi_foundation_ui_status_t status = {0};

    /*
     * The protocol and UI model share one short critical section so an
     * observer never sees a new state paired with stale transcript bytes.
     */
    portENTER_CRITICAL(&ctx->protocol_lock);
    status.state = ctx->ui.state;
    status.listening_started_at_us = ctx->ui.listening_started_at_us;
    status.listening_stopped_at_us = ctx->ui.listening_stopped_at_us;
    status.last_error = ctx->ui.last_error;
    status.user_text_truncated = ctx->protocol.user_text.truncated;
    memcpy(
        status.user_text,
        ctx->protocol.user_text.value,
        sizeof(status.user_text));
    status.assistant_text_truncated = ctx->protocol.assistant_text.truncated;
    memcpy(
        status.assistant_text,
        ctx->protocol.assistant_text.value,
        sizeof(status.assistant_text));
    portEXIT_CRITICAL(&ctx->protocol_lock);

    /* Registration is immutable while an operation is active. */
    portENTER_CRITICAL(&s_operation_lock);
    callback = s_ui_status_callback;
    callback_context = s_ui_status_callback_context;
    portEXIT_CRITICAL(&s_operation_lock);

    if (callback != NULL) {
        /* Never invoke application code while holding a foundation lock. */
        callback(&status, callback_context);
    }

    memset(&status, 0, sizeof(status));
}

static void xiaozhi_foundation_set_ui_state(
    xiaozhi_foundation_validation_ctx_t *ctx,
    xiaozhi_foundation_ui_state_t state,
    esp_err_t last_error)
{
    if (ctx == NULL) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    bool should_publish = false;

    portENTER_CRITICAL(&ctx->protocol_lock);

    /* Keep a protocol failure visible through asynchronous disconnect cleanup. */
    if ((ctx->ui.state == XIAOZHI_FOUNDATION_UI_ERROR) &&
        (state != XIAOZHI_FOUNDATION_UI_ERROR)) {
        portEXIT_CRITICAL(&ctx->protocol_lock);
        return;
    }

    /* A fast server reply must not be visually regressed by send-stop return. */
    if ((ctx->ui.state == XIAOZHI_FOUNDATION_UI_RESPONDING) &&
        (state == XIAOZHI_FOUNDATION_UI_PROCESSING)) {
        portEXIT_CRITICAL(&ctx->protocol_lock);
        return;
    }

    ctx->ui.state = state;
    ctx->ui.last_error = (state == XIAOZHI_FOUNDATION_UI_ERROR) ?
        ((last_error == ESP_OK) ? ESP_FAIL : last_error) : ESP_OK;

    if (state == XIAOZHI_FOUNDATION_UI_LISTENING) {
        ctx->ui.listening_started_at_us = now_us;
        ctx->ui.listening_stopped_at_us = 0;
        memset(&ctx->protocol.user_text, 0, sizeof(ctx->protocol.user_text));
        memset(
            &ctx->protocol.assistant_text,
            0,
            sizeof(ctx->protocol.assistant_text));
        ctx->protocol.conversation_turn_complete = false;
    } else if (((state == XIAOZHI_FOUNDATION_UI_PROCESSING) ||
                (state == XIAOZHI_FOUNDATION_UI_RESPONDING) ||
                (state == XIAOZHI_FOUNDATION_UI_READY) ||
                (state == XIAOZHI_FOUNDATION_UI_ERROR)) &&
               (ctx->ui.listening_started_at_us > 0) &&
               (ctx->ui.listening_stopped_at_us == 0)) {
        ctx->ui.listening_stopped_at_us = now_us;
    }

    should_publish = true;
    portEXIT_CRITICAL(&ctx->protocol_lock);

    if (should_publish) {
        xiaozhi_foundation_publish_ui_status(ctx);
    }
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
        /*
         * Do not read source[maximum_length] merely to distinguish an exact
         * boundary-length string from a longer one. Reaching the scan bound is
         * conservatively reported as truncated and keeps this helper truly
         * bounded to maximum_length readable bytes.
         */
        *out_truncated = (length == maximum_length);
    }

    return length;
}

static void xiaozhi_foundation_record_primary_error(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_err_t error)
{
    if ((ctx == NULL) || (error == ESP_OK)) {
        return;
    }

    portENTER_CRITICAL(&ctx->protocol_lock);
    if (!ctx->result.primary_error_recorded) {
        ctx->result.primary_error_recorded = true;
        ctx->result.primary_error = error;
    }
    portEXIT_CRITICAL(&ctx->protocol_lock);
}

static void xiaozhi_foundation_record_cleanup_error(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_err_t error)
{
    if ((ctx == NULL) || (error == ESP_OK)) {
        return;
    }

    portENTER_CRITICAL(&ctx->protocol_lock);
    if (!ctx->result.cleanup_error_recorded) {
        ctx->result.cleanup_error_recorded = true;
        ctx->result.cleanup_error = error;
    }
    portEXIT_CRITICAL(&ctx->protocol_lock);
}

static void xiaozhi_foundation_capture_resource_snapshot(
    xiaozhi_foundation_resource_snapshot_t *snapshot,
    const char *checkpoint)
{
    if ((snapshot == NULL) || (checkpoint == NULL)) {
        return;
    }

    snapshot->internal_free_bytes = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->internal_min_free_bytes = heap_caps_get_minimum_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->internal_largest_block_bytes = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->dma_free_bytes = heap_caps_get_free_size(MALLOC_CAP_DMA);
    snapshot->dma_min_free_bytes = heap_caps_get_minimum_free_size(
        MALLOC_CAP_DMA);
    snapshot->dma_largest_block_bytes = heap_caps_get_largest_free_block(
        MALLOC_CAP_DMA);
    snapshot->psram_free_bytes = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->psram_min_free_bytes = heap_caps_get_minimum_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->psram_largest_block_bytes = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->worker_stack_high_water_words =
        uxTaskGetStackHighWaterMark(NULL);

    ESP_LOGI(
        TAG,
        "RESOURCE[%s] internal_free_bytes=%u internal_min_free_bytes=%u "
        "internal_largest_block_bytes=%u dma_free_bytes=%u "
        "dma_min_free_bytes=%u dma_largest_block_bytes=%u "
        "psram_free_bytes=%u psram_min_free_bytes=%u "
        "psram_largest_block_bytes=%u worker_stack_hwm_words=%u",
        checkpoint,
        (unsigned)snapshot->internal_free_bytes,
        (unsigned)snapshot->internal_min_free_bytes,
        (unsigned)snapshot->internal_largest_block_bytes,
        (unsigned)snapshot->dma_free_bytes,
        (unsigned)snapshot->dma_min_free_bytes,
        (unsigned)snapshot->dma_largest_block_bytes,
        (unsigned)snapshot->psram_free_bytes,
        (unsigned)snapshot->psram_min_free_bytes,
        (unsigned)snapshot->psram_largest_block_bytes,
        (unsigned)snapshot->worker_stack_high_water_words);
}

static esp_err_t xiaozhi_foundation_log_validation_summary(
    xiaozhi_foundation_validation_ctx_t *ctx,
    const char *checkpoint_name,
    const xiaozhi_foundation_resource_snapshot_t *before,
    const xiaozhi_foundation_resource_snapshot_t *after)
{
    if ((ctx == NULL) || (checkpoint_name == NULL) || (before == NULL) ||
        (after == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_foundation_validation_counters_t counters = {0};
    xiaozhi_foundation_validation_result_t result = {0};
    esp_err_t final_result = ESP_OK;

    portENTER_CRITICAL(&ctx->protocol_lock);
    if (ctx->result.primary_error_recorded) {
        final_result = ctx->result.primary_error;
    } else if (ctx->result.cleanup_error_recorded) {
        final_result = ctx->result.cleanup_error;
    }

    if (final_result == ESP_OK) {
        if (ctx->counters.validation_pass_count < UINT32_MAX) {
            ++ctx->counters.validation_pass_count;
        }
    } else if (ctx->counters.validation_fail_count < UINT32_MAX) {
        ++ctx->counters.validation_fail_count;
    }

    counters = ctx->counters;
    result = ctx->result;
    portEXIT_CRITICAL(&ctx->protocol_lock);

    ESP_LOGI(
        TAG,
        "VALIDATION SUMMARY checkpoint=%s result=%s primary_error=%s "
        "cleanup_error=%s",
        checkpoint_name,
        (final_result == ESP_OK) ? "PASS" : "FAIL",
        result.primary_error_recorded ?
            esp_err_to_name(result.primary_error) : "ESP_OK",
        result.cleanup_error_recorded ?
            esp_err_to_name(result.cleanup_error) : "ESP_OK");
    ESP_LOGI(
        TAG,
        "VALIDATION COUNTERS attempts=%u pass=%u fail=%u connected=%u "
        "disconnected=%u audio_opened=%u audio_closed=%u errors=%u "
        "user_text=%u assistant_text=%u turns=%u tts_start=%u "
        "tts_sentence_start=%u tts_stop=%u audio_rx_callbacks=%u "
        "audio_rx_bytes=%llu",
        (unsigned)counters.validation_attempt_count,
        (unsigned)counters.validation_pass_count,
        (unsigned)counters.validation_fail_count,
        (unsigned)counters.connected_event_count,
        (unsigned)counters.disconnected_event_count,
        (unsigned)counters.audio_channel_opened_count,
        (unsigned)counters.audio_channel_closed_count,
        (unsigned)counters.chat_error_count,
        (unsigned)counters.user_text_event_count,
        (unsigned)counters.assistant_text_event_count,
        (unsigned)counters.conversation_turn_complete_count,
        (unsigned)counters.tts_start_count,
        (unsigned)counters.tts_sentence_start_count,
        (unsigned)counters.tts_stop_count,
        (unsigned)counters.audio_rx_callback_count,
        (unsigned long long)counters.audio_rx_total_bytes);
    ESP_LOGI(
        TAG,
        "RESOURCE DELTA after_cleanup-before_xiaozhi internal_free_bytes=%lld "
        "internal_min_free_bytes=%lld internal_largest_block_bytes=%lld "
        "dma_free_bytes=%lld dma_min_free_bytes=%lld "
        "dma_largest_block_bytes=%lld psram_free_bytes=%lld "
        "psram_min_free_bytes=%lld psram_largest_block_bytes=%lld "
        "worker_stack_hwm_words=%lld",
        (long long)after->internal_free_bytes -
            (long long)before->internal_free_bytes,
        (long long)after->internal_min_free_bytes -
            (long long)before->internal_min_free_bytes,
        (long long)after->internal_largest_block_bytes -
            (long long)before->internal_largest_block_bytes,
        (long long)after->dma_free_bytes -
            (long long)before->dma_free_bytes,
        (long long)after->dma_min_free_bytes -
            (long long)before->dma_min_free_bytes,
        (long long)after->dma_largest_block_bytes -
            (long long)before->dma_largest_block_bytes,
        (long long)after->psram_free_bytes -
            (long long)before->psram_free_bytes,
        (long long)after->psram_min_free_bytes -
            (long long)before->psram_min_free_bytes,
        (long long)after->psram_largest_block_bytes -
            (long long)before->psram_largest_block_bytes,
        (long long)after->worker_stack_high_water_words -
            (long long)before->worker_stack_high_water_words);

    return final_result;
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
        bool non_empty = false;
        bool conversation_turn_completed_now = false;

        if ((text_data == NULL) || (text_data->text == NULL)) {
            return;
        }

        switch (text_data->role) {
        case ESP_XIAOZHI_CHAT_TEXT_ROLE_USER:
            snapshot = &ctx->protocol.user_text;
            event_bit = XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT |
                        XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_USER;
            break;

        case ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT:
            snapshot = &ctx->protocol.assistant_text;
            event_bit = XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT |
                        XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_ASSISTANT;
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
        non_empty = (snapshot->length > 0U);

        if (text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) {
            if (ctx->counters.user_text_event_count < UINT32_MAX) {
                ++ctx->counters.user_text_event_count;
            }
        } else if (ctx->counters.assistant_text_event_count < UINT32_MAX) {
            ++ctx->counters.assistant_text_event_count;
        }

        const bool full_turn_available =
            ctx->protocol.user_text.received &&
            (ctx->protocol.user_text.length > 0U) &&
            ctx->protocol.assistant_text.received &&
            (ctx->protocol.assistant_text.length > 0U);

        if (full_turn_available &&
            !ctx->protocol.conversation_turn_complete) {
            ctx->protocol.conversation_turn_complete = true;
            conversation_turn_completed_now = true;
            if (ctx->counters.conversation_turn_complete_count < UINT32_MAX) {
                ++ctx->counters.conversation_turn_complete_count;
            }
        }
        portEXIT_CRITICAL(&ctx->protocol_lock);

        /* P2-F requires non-empty STT/assistant evidence, not just an event. */
        if (!non_empty) {
            event_bit &= ~XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_USER;
            event_bit &= ~XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_ASSISTANT;
        }

        if (conversation_turn_completed_now) {
            event_bit |= XIAOZHI_FOUNDATION_EVENT_CONVERSATION_TURN_COMPLETE;
        }

        (void)xEventGroupSetBits(ctx->events, event_bit);

        if (text_data->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_ASSISTANT) {
            xiaozhi_foundation_set_ui_state(
                ctx,
                XIAOZHI_FOUNDATION_UI_RESPONDING,
                ESP_OK);
        } else {
            xiaozhi_foundation_publish_ui_status(ctx);
        }
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

        EventBits_t tts_event_bit = XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STATE;

        portENTER_CRITICAL(&ctx->protocol_lock);
        ctx->protocol.last_tts_state = tts_state->state;
        ctx->protocol.tts_state_received = true;

        switch (tts_state->state) {
        case ESP_XIAOZHI_CHAT_TTS_STATE_START:
            ctx->protocol.tts_start_received = true;
            if (ctx->counters.tts_start_count < UINT32_MAX) {
                ++ctx->counters.tts_start_count;
            }
            tts_event_bit |= XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_START;
            break;

        case ESP_XIAOZHI_CHAT_TTS_STATE_SENTENCE_START:
            ctx->protocol.tts_sentence_start_received = true;
            if (ctx->counters.tts_sentence_start_count < UINT32_MAX) {
                ++ctx->counters.tts_sentence_start_count;
            }
            tts_event_bit |= XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_SENTENCE_START;
            break;

        case ESP_XIAOZHI_CHAT_TTS_STATE_STOP:
            ctx->protocol.tts_stop_received = true;
            if (ctx->counters.tts_stop_count < UINT32_MAX) {
                ++ctx->counters.tts_stop_count;
            }
            tts_event_bit |= XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STOP;
            break;

        default:
            /* Validated immediately above; keep compiler flow explicit. */
            break;
        }
        portEXIT_CRITICAL(&ctx->protocol_lock);

        (void)xEventGroupSetBits(ctx->events, tts_event_bit);

        if (tts_state->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) {
            xiaozhi_foundation_set_ui_state(
                ctx,
                XIAOZHI_FOUNDATION_UI_READY,
                ESP_OK);
        } else {
            xiaozhi_foundation_set_ui_state(
                ctx,
                XIAOZHI_FOUNDATION_UI_RESPONDING,
                ESP_OK);
        }
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *const error_info =
            (const esp_xiaozhi_chat_error_info_t *)event_data;

        if (error_info == NULL) {
            return;
        }

        portENTER_CRITICAL(&ctx->protocol_lock);
        if (ctx->counters.chat_error_count < UINT32_MAX) {
            ++ctx->counters.chat_error_count;
        }
        if (!ctx->protocol.first_error_received) {
            ctx->protocol.first_error_code = error_info->code;
            (void)xiaozhi_foundation_copy_bounded_string(
                ctx->protocol.first_error_source,
                sizeof(ctx->protocol.first_error_source),
                error_info->source,
                &ctx->protocol.first_error_source_truncated);
            ctx->protocol.first_error_received = true;
        }
        portEXIT_CRITICAL(&ctx->protocol_lock);

        xiaozhi_foundation_record_primary_error(ctx, error_info->code);

        (void)xEventGroupSetBits(
            ctx->events,
            XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR);
        xiaozhi_foundation_set_ui_state(
            ctx,
            XIAOZHI_FOUNDATION_UI_ERROR,
            error_info->code);
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

static void xiaozhi_foundation_audio_callback(
    const uint8_t *data,
    int len,
    void *callback_ctx)
{
    xiaozhi_foundation_validation_ctx_t *ctx =
        (xiaozhi_foundation_validation_ctx_t *)callback_ctx;

    if ((ctx == NULL) || (ctx->events == NULL) || (data == NULL) ||
        (len <= 0)) {
        return;
    }

    /*
     * The source pointer is valid only for this callback. P2-F deliberately
     * records scalar evidence only: no logging, allocation, or ownership
     * transfer occurs on the binary receive path.
     */
    const size_t packet_size = (size_t)len;

    portENTER_CRITICAL(&ctx->protocol_lock);

    if (ctx->counters.audio_rx_callback_count == 0U) {
        ctx->protocol.audio_rx_first_packet_size = packet_size;
    }

    if (ctx->counters.audio_rx_callback_count < UINT32_MAX) {
        ++ctx->counters.audio_rx_callback_count;
    }

    if (packet_size > ctx->protocol.audio_rx_max_packet_size) {
        ctx->protocol.audio_rx_max_packet_size = packet_size;
    }

    if ((uint64_t)packet_size <=
        (UINT64_MAX - ctx->counters.audio_rx_total_bytes)) {
        ctx->counters.audio_rx_total_bytes += (uint64_t)packet_size;
    } else {
        ctx->counters.audio_rx_total_bytes = UINT64_MAX;
    }

    portEXIT_CRITICAL(&ctx->protocol_lock);

    (void)xEventGroupSetBits(ctx->events, XIAOZHI_FOUNDATION_EVENT_AUDIO_RX);
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
                          XIAOZHI_FOUNDATION_EVENT_CHAT_EMOJI |
                          XIAOZHI_FOUNDATION_EVENT_CONVERSATION_TURN_COMPLETE)) == 0U) {
        return;
    }

    bool user_text_received = false;
    size_t user_text_length = 0U;
    bool user_text_truncated = false;
    bool assistant_text_received = false;
    size_t assistant_text_length = 0U;
    bool assistant_text_truncated = false;
    bool conversation_turn_complete = false;
    bool tts_state_received = false;
    esp_xiaozhi_chat_tts_state_kind_t last_tts_state =
        ESP_XIAOZHI_CHAT_TTS_STATE_START;
    bool first_error_received = false;
    esp_err_t first_error_code = ESP_OK;
    char first_error_source[
        XIAOZHI_FOUNDATION_CHAT_ERROR_SOURCE_BUFFER_SIZE] = {0};
    bool first_error_source_truncated = false;
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
    conversation_turn_complete = ctx->protocol.conversation_turn_complete;
    tts_state_received = ctx->protocol.tts_state_received;
    last_tts_state = ctx->protocol.last_tts_state;
    first_error_received = ctx->protocol.first_error_received;
    first_error_code = ctx->protocol.first_error_code;
    first_error_source_truncated =
        ctx->protocol.first_error_source_truncated;
    (void)xiaozhi_foundation_copy_bounded_string(
        first_error_source,
        sizeof(first_error_source),
        ctx->protocol.first_error_source,
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

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CONVERSATION_TURN_COMPLETE) != 0U) &&
        conversation_turn_complete) {
        ESP_LOGI(TAG, "Conversation turn complete: USER -> ASSISTANT");
    }

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STATE) != 0U) &&
        tts_state_received) {
        ESP_LOGI(
            TAG,
            "TTS state=%s",
            xiaozhi_foundation_tts_state_to_string(last_tts_state));
    }

    if (((protocol_bits & XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR) != 0U) &&
        first_error_received) {
        ESP_LOGW(
            TAG,
            "CHAT_ERROR first_code=%s source=%s%s",
            esp_err_to_name(first_error_code),
            (first_error_source[0] != '\0') ? first_error_source : "unknown",
            first_error_source_truncated ? " truncated" : "");
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

static xiaozhi_foundation_validation_checkpoint_t
xiaozhi_foundation_get_validation_checkpoint(void)
{
#if CONFIG_XIAOZHI_FOUNDATION_P2F_E2E_ONLINE_VALIDATION
    return XIAOZHI_FOUNDATION_VALIDATION_P2F_AUDIO_E2E;
#else
    return XIAOZHI_FOUNDATION_VALIDATION_P2E_AUDIO_CHANNEL;
#endif
}

static const char *xiaozhi_foundation_validation_checkpoint_to_string(
    xiaozhi_foundation_validation_checkpoint_t checkpoint)
{
    switch (checkpoint) {
    case XIAOZHI_FOUNDATION_VALIDATION_P2E_AUDIO_CHANNEL:
        return "P2-E";

    case XIAOZHI_FOUNDATION_VALIDATION_P2F_AUDIO_E2E:
        return "P2-F";

    default:
        return "UNKNOWN";
    }
}

static esp_err_t xiaozhi_foundation_get_runtime_error(
    xiaozhi_foundation_validation_ctx_t *ctx,
    const char *operation)
{
    if ((ctx == NULL) || (ctx->events == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const EventBits_t bits = xEventGroupGetBits(ctx->events);

    if ((bits & XIAOZHI_FOUNDATION_EVENT_DISCONNECTED) != 0U) {
        ESP_LOGE(TAG, "%s: DISCONNECTED", operation);
        return ESP_FAIL;
    }

    if ((bits & XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR) != 0U) {
        esp_err_t error_code = ESP_FAIL;
        char error_source[
            XIAOZHI_FOUNDATION_CHAT_ERROR_SOURCE_BUFFER_SIZE] = {0};
        bool error_source_truncated = false;

        portENTER_CRITICAL(&ctx->protocol_lock);
        if (ctx->protocol.first_error_received &&
            (ctx->protocol.first_error_code != ESP_OK)) {
            error_code = ctx->protocol.first_error_code;
        }
        error_source_truncated =
            ctx->protocol.first_error_source_truncated;
        (void)xiaozhi_foundation_copy_bounded_string(
            error_source,
            sizeof(error_source),
            ctx->protocol.first_error_source,
            NULL);
        portEXIT_CRITICAL(&ctx->protocol_lock);

        ESP_LOGE(
            TAG,
            "%s: CHAT_ERROR code=%s source=%s%s",
            operation,
            esp_err_to_name(error_code),
            (error_source[0] != '\0') ? error_source : "unknown",
            error_source_truncated ? " truncated" : "");
        return error_code;
    }

    return ESP_OK;
}

static esp_err_t xiaozhi_foundation_wait_for_event(
    xiaozhi_foundation_validation_ctx_t *ctx,
    EventBits_t expected,
    uint32_t timeout_ms,
    const char *operation)
{
    if ((ctx == NULL) || (ctx->events == NULL) || (expected == 0U) ||
        (operation == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const EventBits_t observed = xEventGroupWaitBits(
        ctx->events,
        expected |
            XIAOZHI_FOUNDATION_EVENT_DISCONNECTED |
            XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    const esp_err_t runtime_ret =
        xiaozhi_foundation_get_runtime_error(ctx, operation);
    if (runtime_ret != ESP_OK) {
        return runtime_ret;
    }

    if ((observed & expected) == 0U) {
        ESP_LOGE(TAG, "%s: timeout after %u ms", operation,
                 (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t xiaozhi_foundation_open_audio_channel(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open)
{
    if ((ctx == NULL) || (ctx->events == NULL) || (chat == 0) ||
        (audio_channel_open == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_xiaozhi_chat_audio_t audio = {
        .format = XIAOZHI_FOUNDATION_AUDIO_FORMAT,
        .sample_rate = XIAOZHI_FOUNDATION_AUDIO_SAMPLE_RATE,
        .channels = XIAOZHI_FOUNDATION_AUDIO_CHANNELS,
        .frame_duration = XIAOZHI_FOUNDATION_AUDIO_FRAME_DURATION_MS,
    };

    /*
     * Clear only checkpoint-local observations. DISCONNECTED and CHAT_ERROR
     * are sticky runtime-failure facts and must never be erased between steps.
     */
    xEventGroupClearBits(
        ctx->events,
        XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_OPENED |
            XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_CLOSED);

    ESP_LOGI(TAG, "Opening audio channel");

    esp_err_t ret = esp_xiaozhi_chat_open_audio_channel(
        chat,
        &audio,
        NULL,
        0U);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "open_audio_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *audio_channel_open = true;
    ESP_LOGI(TAG, "open_audio_channel: OK");

    ret = xiaozhi_foundation_wait_for_event(
        ctx,
        XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_OPENED,
        XIAOZHI_FOUNDATION_AUDIO_CHANNEL_OPEN_TIMEOUT_MS,
        "AUDIO_CHANNEL_OPENED");
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "AUDIO_CHANNEL_OPENED");
    return ESP_OK;
}

static esp_err_t xiaozhi_foundation_close_audio_channel(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open,
    bool cleanup)
{
    if ((ctx == NULL) || (ctx->events == NULL) || (chat == 0) ||
        (audio_channel_open == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!*audio_channel_open) {
        return ESP_OK;
    }

    xEventGroupClearBits(ctx->events,
                         XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_CLOSED);

    if (cleanup) {
        ESP_LOGI(TAG, "Cleanup closing audio channel");
    } else {
        ESP_LOGI(TAG, "Closing audio channel");
    }

    esp_err_t ret = esp_xiaozhi_chat_close_audio_channel(chat);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "close_audio_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *audio_channel_open = false;
    ESP_LOGI(TAG, "close_audio_channel: OK");

    ret = xiaozhi_foundation_wait_for_event(
        ctx,
        XIAOZHI_FOUNDATION_EVENT_AUDIO_CHANNEL_CLOSED,
        XIAOZHI_FOUNDATION_AUDIO_CHANNEL_CLOSE_TIMEOUT_MS,
        "AUDIO_CHANNEL_CLOSED");
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "AUDIO_CHANNEL_CLOSED");
    return ESP_OK;
}

static esp_err_t xiaozhi_foundation_validate_p2e_audio_channel(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open)
{
    ESP_LOGI(TAG, "=== P2-E WEBSOCKET AUDIO CHANNEL ===");

    esp_err_t ret = xiaozhi_foundation_open_audio_channel(
        ctx,
        chat,
        audio_channel_open);
    if (ret != ESP_OK) {
        return ret;
    }

    const EventBits_t hold_bits = xEventGroupWaitBits(
        ctx->events,
        XIAOZHI_FOUNDATION_EVENT_DISCONNECTED |
            XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(XIAOZHI_FOUNDATION_AUDIO_CHANNEL_HOLD_MS));
    (void)hold_bits;

    ret = xiaozhi_foundation_get_runtime_error(ctx, "P2-E audio hold");
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Audio channel stable for %u ms",
             (unsigned)XIAOZHI_FOUNDATION_AUDIO_CHANNEL_HOLD_MS);

    return xiaozhi_foundation_close_audio_channel(
        ctx,
        chat,
        audio_channel_open,
        false);
}

static uint16_t xiaozhi_foundation_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint32_t xiaozhi_foundation_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static esp_err_t xiaozhi_foundation_get_p2f_fixture(
    const uint8_t **out_data,
    size_t *out_size)
{
    if ((out_data == NULL) || (out_size == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_size = 0U;

#if CONFIG_XIAOZHI_FOUNDATION_P2F_EMBED_FIXTURE
    if ((uintptr_t)xiaozhi_p2f_fixture_end <=
        (uintptr_t)xiaozhi_p2f_fixture_start) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_data = xiaozhi_p2f_fixture_start;
    *out_size = (size_t)((uintptr_t)xiaozhi_p2f_fixture_end -
                         (uintptr_t)xiaozhi_p2f_fixture_start);
    return ESP_OK;
#else
    return ESP_ERR_NOT_FOUND;
#endif
}

static esp_err_t xiaozhi_foundation_parse_p2f_fixture(
    const uint8_t *data,
    size_t data_size,
    xiaozhi_foundation_p2f_fixture_t *out_fixture)
{
    if ((data == NULL) || (out_fixture == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_fixture, 0, sizeof(*out_fixture));

    if ((data_size < XIAOZHI_FOUNDATION_P2F_FIXTURE_HEADER_SIZE) ||
        (data_size > XIAOZHI_FOUNDATION_P2F_MAX_FIXTURE_BYTES)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if ((memcmp(data, XIAOZHI_FOUNDATION_P2F_FIXTURE_MAGIC, 4U) != 0) ||
        (data[4] != XIAOZHI_FOUNDATION_P2F_FIXTURE_VERSION) ||
        (data[5] != XIAOZHI_FOUNDATION_P2F_FIXTURE_CODEC_OPUS) ||
        (xiaozhi_foundation_read_le16(&data[6]) !=
         XIAOZHI_FOUNDATION_P2F_FIXTURE_HEADER_SIZE) ||
        (xiaozhi_foundation_read_le32(&data[8]) !=
         XIAOZHI_FOUNDATION_AUDIO_SAMPLE_RATE) ||
        (data[12] != XIAOZHI_FOUNDATION_AUDIO_CHANNELS) ||
        (data[13] != 0U) ||
        (xiaozhi_foundation_read_le16(&data[14]) !=
         XIAOZHI_FOUNDATION_AUDIO_FRAME_DURATION_MS)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t frame_count = xiaozhi_foundation_read_le32(&data[16]);
    const uint32_t records_size = xiaozhi_foundation_read_le32(&data[20]);

    if ((frame_count == 0U) ||
        (frame_count > XIAOZHI_FOUNDATION_P2F_MAX_FRAMES) ||
        ((size_t)records_size !=
         (data_size - XIAOZHI_FOUNDATION_P2F_FIXTURE_HEADER_SIZE))) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *const first_frame =
        data + XIAOZHI_FOUNDATION_P2F_FIXTURE_HEADER_SIZE;
    const uint8_t *const end = data + data_size;
    const uint8_t *cursor = first_frame;

    /* Validate the complete fixed-duration frame table before transmitting. */
    for (uint32_t index = 0U; index < frame_count; ++index) {
        if ((size_t)(end - cursor) < sizeof(uint16_t)) {
            return ESP_ERR_INVALID_SIZE;
        }

        const uint16_t frame_size = xiaozhi_foundation_read_le16(cursor);
        cursor += sizeof(uint16_t);

        if ((frame_size == 0U) ||
            (frame_size > XIAOZHI_FOUNDATION_P2F_MAX_FRAME_BYTES) ||
            ((size_t)(end - cursor) < (size_t)frame_size)) {
            return ESP_ERR_INVALID_SIZE;
        }

        cursor += frame_size;
    }

    if (cursor != end) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_fixture->first_frame = first_frame;
    out_fixture->end = end;
    out_fixture->frame_count = frame_count;
    out_fixture->frame_duration_ms =
        XIAOZHI_FOUNDATION_AUDIO_FRAME_DURATION_MS;
    return ESP_OK;
}

static esp_err_t xiaozhi_foundation_send_p2f_fixture(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    const xiaozhi_foundation_p2f_fixture_t *fixture,
    uint32_t *out_frames_sent,
    size_t *out_bytes_sent)
{
    if ((ctx == NULL) || (chat == 0) || (fixture == NULL) ||
        (fixture->first_frame == NULL) || (fixture->end == NULL) ||
        (out_frames_sent == NULL) || (out_bytes_sent == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_frames_sent = 0U;
    *out_bytes_sent = 0U;

    const uint8_t *cursor = fixture->first_frame;

    for (uint32_t index = 0U; index < fixture->frame_count; ++index) {
        esp_err_t ret = xiaozhi_foundation_get_runtime_error(
            ctx,
            "P2-F audio TX");
        if (ret != ESP_OK) {
            return ret;
        }

        /* The fixture parser has already validated every record boundary. */
        if ((size_t)(fixture->end - cursor) < sizeof(uint16_t)) {
            return ESP_ERR_INVALID_SIZE;
        }

        const uint16_t frame_size = xiaozhi_foundation_read_le16(cursor);
        cursor += sizeof(uint16_t);

        if ((frame_size == 0U) ||
            ((size_t)(fixture->end - cursor) < (size_t)frame_size)) {
            return ESP_ERR_INVALID_SIZE;
        }

        ret = esp_xiaozhi_chat_send_audio_data(
            chat,
            (const char *)cursor,
            frame_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio TX frame %u/%u failed: %s",
                     (unsigned)(index + 1U),
                     (unsigned)fixture->frame_count,
                     esp_err_to_name(ret));
            return ret;
        }

        ++(*out_frames_sent);
        *out_bytes_sent += frame_size;
        cursor += frame_size;

        if ((index + 1U) < fixture->frame_count) {
            vTaskDelay(pdMS_TO_TICKS(fixture->frame_duration_ms));
        }
    }

    return ESP_OK;
}

static esp_err_t xiaozhi_foundation_wait_for_p2f_evidence(
    xiaozhi_foundation_validation_ctx_t *ctx)
{
    if ((ctx == NULL) || (ctx->events == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const EventBits_t required =
        XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_USER |
        XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_ASSISTANT |
        XIAOZHI_FOUNDATION_EVENT_CONVERSATION_TURN_COMPLETE |
        XIAOZHI_FOUNDATION_EVENT_AUDIO_RX;
    const TickType_t timeout_ticks =
        pdMS_TO_TICKS(XIAOZHI_FOUNDATION_P2F_RESPONSE_TIMEOUT_MS);
    const TickType_t started_at = xTaskGetTickCount();

    for (;;) {
        const esp_err_t runtime_ret = xiaozhi_foundation_get_runtime_error(
            ctx,
            "P2-F protocol response");
        if (runtime_ret != ESP_OK) {
            return runtime_ret;
        }

        const EventBits_t current = xEventGroupGetBits(ctx->events);
        if ((current & required) == required) {
            return ESP_OK;
        }

        const TickType_t elapsed = xTaskGetTickCount() - started_at;
        if (elapsed >= timeout_ticks) {
            ESP_LOGE(TAG, "P2-F response timeout after %u ms",
                     (unsigned)XIAOZHI_FOUNDATION_P2F_RESPONSE_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }

        const EventBits_t missing = required & ~current;
        const TickType_t remaining = timeout_ticks - elapsed;

        (void)xEventGroupWaitBits(
            ctx->events,
            missing |
                XIAOZHI_FOUNDATION_EVENT_DISCONNECTED |
                XIAOZHI_FOUNDATION_EVENT_CHAT_ERROR,
            pdFALSE,
            pdFALSE,
            remaining);
    }
}

static void xiaozhi_foundation_copy_log_safe_text(
    char *destination,
    size_t destination_size,
    const char *source)
{
    if ((destination == NULL) || (destination_size == 0U)) {
        return;
    }

    destination[0] = '\0';
    if (source == NULL) {
        return;
    }

    size_t index = 0U;
    while ((index < (destination_size - 1U)) && (source[index] != '\0')) {
        const unsigned char value = (unsigned char)source[index];
        destination[index] = ((value >= 0x20U) && (value <= 0x7eU) &&
                              (value != (unsigned char)'"') &&
                              (value != (unsigned char)'\\')) ?
                                 (char)value :
                                 '?';
        ++index;
    }

    destination[index] = '\0';
}

static void xiaozhi_foundation_log_p2f_evidence(
    xiaozhi_foundation_validation_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    bool user_text_received = false;
    bool user_text_truncated = false;
    size_t user_text_length = 0U;
    char user_text[XIAOZHI_FOUNDATION_CHAT_TEXT_BUFFER_SIZE] = {0};
    bool assistant_text_received = false;
    bool assistant_text_truncated = false;
    size_t assistant_text_length = 0U;
    char assistant_text[XIAOZHI_FOUNDATION_CHAT_TEXT_BUFFER_SIZE] = {0};
    bool conversation_turn_complete = false;
    bool tts_start_received = false;
    bool tts_sentence_start_received = false;
    bool tts_stop_received = false;
    uint32_t audio_rx_callback_count = 0U;
    uint64_t audio_rx_total_bytes = 0U;
    size_t audio_rx_first_packet_size = 0U;
    size_t audio_rx_max_packet_size = 0U;

    portENTER_CRITICAL(&ctx->protocol_lock);
    user_text_received = ctx->protocol.user_text.received;
    user_text_truncated = ctx->protocol.user_text.truncated;
    user_text_length = ctx->protocol.user_text.length;
    (void)xiaozhi_foundation_copy_bounded_string(
        user_text,
        sizeof(user_text),
        ctx->protocol.user_text.value,
        NULL);
    assistant_text_received = ctx->protocol.assistant_text.received;
    assistant_text_truncated = ctx->protocol.assistant_text.truncated;
    assistant_text_length = ctx->protocol.assistant_text.length;
    (void)xiaozhi_foundation_copy_bounded_string(
        assistant_text,
        sizeof(assistant_text),
        ctx->protocol.assistant_text.value,
        NULL);
    conversation_turn_complete = ctx->protocol.conversation_turn_complete;
    tts_start_received = ctx->protocol.tts_start_received;
    tts_sentence_start_received = ctx->protocol.tts_sentence_start_received;
    tts_stop_received = ctx->protocol.tts_stop_received;
    audio_rx_callback_count = ctx->counters.audio_rx_callback_count;
    audio_rx_total_bytes = ctx->counters.audio_rx_total_bytes;
    audio_rx_first_packet_size = ctx->protocol.audio_rx_first_packet_size;
    audio_rx_max_packet_size = ctx->protocol.audio_rx_max_packet_size;
    portEXIT_CRITICAL(&ctx->protocol_lock);

    if (user_text_received) {
        char safe_text[XIAOZHI_FOUNDATION_P2F_LOG_TEXT_BUFFER_SIZE] = {0};
        xiaozhi_foundation_copy_log_safe_text(
            safe_text,
            sizeof(safe_text),
            user_text);
        ESP_LOGI(TAG, "CHAT_TEXT role=USER len=%u%s",
                 (unsigned)user_text_length,
                 user_text_truncated ? " truncated" : "");
        ESP_LOGI(TAG, "USER text=\"%s\"", safe_text);
        memset(safe_text, 0, sizeof(safe_text));
    }

    if (assistant_text_received) {
        char safe_text[XIAOZHI_FOUNDATION_P2F_LOG_TEXT_BUFFER_SIZE] = {0};
        xiaozhi_foundation_copy_log_safe_text(
            safe_text,
            sizeof(safe_text),
            assistant_text);
        ESP_LOGI(TAG, "CHAT_TEXT role=ASSISTANT len=%u%s",
                 (unsigned)assistant_text_length,
                 assistant_text_truncated ? " truncated" : "");
        ESP_LOGI(TAG, "ASSISTANT text=\"%s\"", safe_text);
        memset(safe_text, 0, sizeof(safe_text));
    }

    if (conversation_turn_complete) {
        ESP_LOGI(TAG, "Conversation Q/A text turn: COMPLETE");
    }

    if (tts_start_received) {
        ESP_LOGI(TAG, "TTS state=START");
    }
    if (tts_sentence_start_received) {
        ESP_LOGI(TAG, "TTS state=SENTENCE_START");
    }
    if (tts_stop_received) {
        ESP_LOGI(TAG, "TTS state=STOP");
    }

    ESP_LOGI(TAG, "Audio RX packets=%u bytes=%llu first=%u max=%u",
              (unsigned)audio_rx_callback_count,
              (unsigned long long)audio_rx_total_bytes,
             (unsigned)audio_rx_first_packet_size,
             (unsigned)audio_rx_max_packet_size);

    memset(user_text, 0, sizeof(user_text));
    memset(assistant_text, 0, sizeof(assistant_text));
}

static esp_err_t xiaozhi_foundation_validate_p2f_audio_e2e(
    xiaozhi_foundation_validation_ctx_t *ctx,
    esp_xiaozhi_chat_handle_t chat,
    bool *audio_channel_open)
{
    if ((ctx == NULL) || (ctx->events == NULL) || (chat == 0) ||
        (audio_channel_open == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "=== P2-F WEBSOCKET AUDIO E2E ===");

    const uint8_t *fixture_data = NULL;
    size_t fixture_size = 0U;
    xiaozhi_foundation_p2f_fixture_t fixture = {0};
    esp_err_t ret = xiaozhi_foundation_get_p2f_fixture(
        &fixture_data,
        &fixture_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P2-F fixture is not embedded: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = xiaozhi_foundation_parse_p2f_fixture(
        fixture_data,
        fixture_size,
        &fixture);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "P2-F fixture format rejected: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = xiaozhi_foundation_open_audio_channel(
        ctx,
        chat,
        audio_channel_open);
    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * Start a fresh interaction evidence window, but keep transport/protocol
     * failures sticky so a race cannot be converted into a misleading timeout.
     */
    xEventGroupClearBits(
        ctx->events,
        XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT |
            XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_USER |
            XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_ASSISTANT |
            XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STATE |
            XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_START |
            XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_SENTENCE_START |
            XIAOZHI_FOUNDATION_EVENT_CHAT_TTS_STOP |
            XIAOZHI_FOUNDATION_EVENT_AUDIO_RX |
            XIAOZHI_FOUNDATION_EVENT_AUDIO_DATA_INCOMING |
            XIAOZHI_FOUNDATION_EVENT_CONVERSATION_TURN_COMPLETE);

    ret = xiaozhi_foundation_get_runtime_error(ctx, "P2-F start");
    if (ret != ESP_OK) {
        return ret;
    }

    bool listening_started = false;
    uint32_t frames_sent = 0U;
    size_t bytes_sent = 0U;

    ret = esp_xiaozhi_chat_send_start_listening(
        chat,
        ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_listening failed: %s", esp_err_to_name(ret));
        return ret;
    }

    listening_started = true;
    xiaozhi_foundation_set_ui_state(
        ctx,
        XIAOZHI_FOUNDATION_UI_LISTENING,
        ESP_OK);
    ESP_LOGI(TAG, "start_listening: OK");
    ESP_LOGI(TAG, "Sending known audio");

    ret = xiaozhi_foundation_send_p2f_fixture(
        ctx,
        chat,
        &fixture,
        &frames_sent,
        &bytes_sent);
    ESP_LOGI(TAG, "Audio TX frames=%u bytes=%u",
             (unsigned)frames_sent,
             (unsigned)bytes_sent);

    if (listening_started) {
        const esp_err_t stop_ret = esp_xiaozhi_chat_send_stop_listening(chat);
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "stop_listening failed: %s",
                     esp_err_to_name(stop_ret));
            if (ret == ESP_OK) {
                ret = stop_ret;
            }
        } else {
            xiaozhi_foundation_set_ui_state(
                ctx,
                XIAOZHI_FOUNDATION_UI_PROCESSING,
                ESP_OK);
            ESP_LOGI(TAG, "stop_listening: OK");
        }
    }

    if (ret == ESP_OK) {
        ret = xiaozhi_foundation_wait_for_p2f_evidence(ctx);
    }

    xiaozhi_foundation_log_p2f_evidence(ctx);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Conversation Q/A text evidence: PASS");
    }

    if (ret == ESP_ERR_TIMEOUT) {
        const EventBits_t evidence = xEventGroupGetBits(ctx->events);
        if ((evidence & XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_USER) == 0U) {
            ESP_LOGE(TAG, "P2-F FAIL: no USER CHAT_TEXT");
        }
        if ((evidence & XIAOZHI_FOUNDATION_EVENT_CHAT_TEXT_ASSISTANT) == 0U) {
            ESP_LOGE(TAG, "P2-F FAIL: no ASSISTANT CHAT_TEXT");
        }
        if ((evidence & XIAOZHI_FOUNDATION_EVENT_CONVERSATION_TURN_COMPLETE) == 0U) {
            ESP_LOGE(TAG, "P2-F FAIL: no complete USER -> ASSISTANT conversation turn");
        }
        if ((evidence & XIAOZHI_FOUNDATION_EVENT_AUDIO_RX) == 0U) {
            ESP_LOGE(TAG, "P2-F FAIL: no audio callback data");
        }
    }

    return ret;
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

    const xiaozhi_foundation_validation_checkpoint_t checkpoint =
        xiaozhi_foundation_get_validation_checkpoint();
    const char *const checkpoint_name =
        xiaozhi_foundation_validation_checkpoint_to_string(checkpoint);

    ESP_LOGI(
        TAG,
        "Transport requested: %s",
        xiaozhi_foundation_transport_to_string(requested));
    ESP_LOGI(TAG, "Validation checkpoint selected: %s", checkpoint_name);

    xiaozhi_foundation_validation_ctx_t ctx = {
        .requested_transport = requested,
        .events = NULL,
        .protocol_lock = portMUX_INITIALIZER_UNLOCKED,
        .ui = {
            .state = XIAOZHI_FOUNDATION_UI_DISCONNECTED,
            .last_error = ESP_OK,
        },
    };
    xiaozhi_foundation_resource_snapshot_t before_xiaozhi = {0};
    xiaozhi_foundation_resource_snapshot_t after_cleanup = {0};
    esp_xiaozhi_chat_info_t info = {0};
    esp_mcp_t *mcp = NULL;
    esp_xiaozhi_chat_handle_t chat = 0;
    bool chat_started = false;
    bool audio_channel_open = false;
    bool event_handler_registered = false;
    esp_event_handler_instance_t event_handler_instance = NULL;
    xiaozhi_foundation_transport_t selected_transport =
        XIAOZHI_FOUNDATION_TRANSPORT_AUTO;
    esp_err_t ret = ESP_OK;

    ctx.counters.validation_attempt_count = 1U;
    xiaozhi_foundation_capture_resource_snapshot(
        &before_xiaozhi,
        "BEFORE_XIAOZHI");
    xiaozhi_foundation_publish_ui_status(&ctx);

    ret = esp_xiaozhi_chat_get_info(&info);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to get Xiaozhi service info: %s",
            esp_err_to_name(ret));

        /* get_info() may have partially allocated response fields. */
        const esp_err_t free_info_ret = esp_xiaozhi_chat_free_info(&info);
        if (free_info_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to free partial Xiaozhi service info: %s",
                     esp_err_to_name(free_info_ret));
            xiaozhi_foundation_record_cleanup_error(&ctx, free_info_ret);
        }
        goto cleanup;
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
        goto cleanup;
    }

    ESP_LOGI(
        TAG,
        "WebSocket available: %s",
        ctx.websocket_available ? "yes" : "no");

    ret = xiaozhi_foundation_select_transport(
        &ctx,
        &selected_transport);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "No usable Xiaozhi WebSocket transport: %s",
            esp_err_to_name(ret));
        goto cleanup;
    }

    ESP_LOGI(
        TAG,
        "Transport selected: %s",
        xiaozhi_foundation_transport_to_string(
            selected_transport));

    ret = esp_mcp_create(&mcp);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create MCP engine: %s",
            esp_err_to_name(ret));
        goto cleanup;
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
    chat_config.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    chat_config.audio_callback = xiaozhi_foundation_audio_callback;
    chat_config.audio_callback_ctx = &ctx;

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
    xiaozhi_foundation_capture_resource_snapshot(
        &(xiaozhi_foundation_resource_snapshot_t){0},
        "AFTER_CHAT_INIT");

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

    ret = xiaozhi_foundation_wait_for_event(
        &ctx,
        XIAOZHI_FOUNDATION_EVENT_CONNECTED,
        XIAOZHI_FOUNDATION_CONNECT_TIMEOUT_MS,
        "CONNECTED");
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ESP_LOGI(TAG, "CONNECTED event received");
    ESP_LOGI(
        TAG,
        "Transport connected: %s",
        xiaozhi_foundation_transport_to_string(
            selected_transport));
    ESP_LOGI(TAG, "WebSocket connected");
    xiaozhi_foundation_capture_resource_snapshot(
        &(xiaozhi_foundation_resource_snapshot_t){0},
        "AFTER_CONNECTED");

    if (checkpoint == XIAOZHI_FOUNDATION_VALIDATION_P2E_AUDIO_CHANNEL) {
        ret = xiaozhi_foundation_validate_p2e_audio_channel(
            &ctx,
            chat,
            &audio_channel_open);
    } else {
        ret = xiaozhi_foundation_validate_p2f_audio_e2e(
            &ctx,
            chat,
            &audio_channel_open);

        if (audio_channel_open) {
            const esp_err_t close_ret =
                xiaozhi_foundation_close_audio_channel(
                    &ctx,
                    chat,
                    &audio_channel_open,
                    false);

            if ((ret == ESP_OK) && (close_ret != ESP_OK)) {
                ret = close_ret;
            }
        }
    }

cleanup:
    if (ret != ESP_OK) {
        xiaozhi_foundation_record_primary_error(&ctx, ret);
    }

    xiaozhi_foundation_capture_resource_snapshot(
        &(xiaozhi_foundation_resource_snapshot_t){0},
        "AFTER_VALIDATION");

    /*
     * Keep the direct protocol-callback context and EventGroup alive until
     * chat_deinit() returns. The ESP event handler may be unregistered after
     * chat_stop(); direct esp_xiaozhi callbacks can still refer to ctx while
     * deinitialization releases the chat-owned runtime.
     */
    if (audio_channel_open) {
        const esp_err_t close_ret = xiaozhi_foundation_close_audio_channel(
            &ctx,
            chat,
            &audio_channel_open,
            true);

        if (close_ret != ESP_OK) {
            xiaozhi_foundation_record_cleanup_error(&ctx, close_ret);
        }
    }

    if (chat_started) {
        const esp_err_t stop_ret =
            esp_xiaozhi_chat_stop(chat);

        if (stop_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "chat_stop failed: %s",
                esp_err_to_name(stop_ret));

            xiaozhi_foundation_record_cleanup_error(&ctx, stop_ret);
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

            xiaozhi_foundation_record_cleanup_error(&ctx, unregister_ret);
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

            xiaozhi_foundation_record_cleanup_error(&ctx, deinit_ret);
        } else {
            ESP_LOGI(TAG, "chat_deinit: OK");
        }
    }

    xiaozhi_foundation_log_protocol_diagnostics(&ctx);

    if (mcp != NULL) {
        const esp_err_t destroy_ret =
            esp_mcp_destroy(mcp);

        if (destroy_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "MCP engine destroy failed: %s",
                esp_err_to_name(destroy_ret));

            xiaozhi_foundation_record_cleanup_error(&ctx, destroy_ret);
        } else {
            ESP_LOGI(TAG, "MCP engine destroyed");
        }
    }

    if (ctx.events != NULL) {
        vEventGroupDelete(ctx.events);
        ctx.events = NULL;
    }

    xiaozhi_foundation_capture_resource_snapshot(
        &after_cleanup,
        "AFTER_CLEANUP");
    const esp_err_t final_ret = xiaozhi_foundation_log_validation_summary(
        &ctx,
        checkpoint_name,
        &before_xiaozhi,
        &after_cleanup);

    if (final_ret == ESP_OK) {
        xiaozhi_foundation_set_ui_state(
            &ctx,
            XIAOZHI_FOUNDATION_UI_DISCONNECTED,
            ESP_OK);
    } else {
        xiaozhi_foundation_set_ui_state(
            &ctx,
            XIAOZHI_FOUNDATION_UI_ERROR,
            final_ret);
    }

    /* Do not leave copied user/assistant text on this short-lived task stack. */
    memset(&ctx.protocol, 0, sizeof(ctx.protocol));
    memset(&ctx.counters, 0, sizeof(ctx.counters));
    memset(&ctx.result, 0, sizeof(ctx.result));

    if (final_ret == ESP_OK) {
        ESP_LOGI(TAG, "%s RESULT: PASS", checkpoint_name);
    } else {
        ESP_LOGE(TAG, "%s RESULT: FAIL: %s", checkpoint_name,
                 esp_err_to_name(final_ret));
    }

    return final_ret;
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

esp_err_t xiaozhi_foundation_register_ui_status_callback(
    xiaozhi_foundation_ui_status_callback_t callback,
    void *user_context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;

    portENTER_CRITICAL(&s_operation_lock);

    if (s_operation_in_progress) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_ui_status_callback = callback;
        s_ui_status_callback_context = user_context;
    }

    portEXIT_CRITICAL(&s_operation_lock);

    return ret;
}
