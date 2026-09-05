#include "voice_assistant_downlink.h"

#include <stdatomic.h>
#include <string.h>

#include "voice_assistant_audio_arbitration_bridge.h"
#include "voice_assistant_opus.h"
#include "xiaozhi_foundation.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DOWNLINK_TASK_NAME                 "voice_downlink"
/* esp_audio_codec documents about 20 KiB of task stack for decoder coverage;
 * keep additional coordinator headroom for queue/stream orchestration. */
#define DOWNLINK_TASK_STACK_BYTES          (24U * 1024U)
/* The upstream WebSocket dispatch task runs at priority 5. Keep the consumer
 * one level higher so a successful callback enqueue immediately runs decode
 * work before dispatch can drain a burst of additional response frames. */
#define DOWNLINK_TASK_PRIORITY             6U
#define DOWNLINK_QUEUE_LENGTH              128U
/* Server packets are complete Opus frames and must never be split. A prior
 * target trace recorded a 2100-byte frame. 2304 B retains that headroom. The
 * 128-frame ring holds 7.68 seconds of 60 ms Opus audio bursts; it resides in
 * PSRAM so a long cloud response cannot exhaust microphone/DMA internal RAM. */
#define DOWNLINK_CHUNK_BYTES               2304U
#define DOWNLINK_RESPONSE_TIMEOUT_MS       15000U
/* Bound the server collection too, so duplicate protocol events or a missing
 * TTS_STOP cannot hold the next PTT turn forever. */
/* Keep a finite turn bound, but allow unusually long cloud answers to finish
 * draining. The independent inactivity and drain timeouts still recover a
 * stalled response before this ten-minute absolute ceiling. */
#define DOWNLINK_RESPONSE_MAX_DURATION_MS 600000U
#define DOWNLINK_QUEUE_POLL_MS             100U
#define DOWNLINK_STREAM_DRAIN_TIMEOUT_MS   90000U
/* The decoded ingress ring is deliberately smaller than the copied WebSocket
 * packet queue. A full ring is transient while the manager starts TX or
 * drains; retain the current complete packet and yield rather than converting
 * normal producer/consumer pressure into a truncated server response. Keep
 * this finite because the public WebSocket callback cannot be flow-controlled
 * by this component. */
#define DOWNLINK_STREAM_BACKPRESSURE_RETRY_MS    20U
#define DOWNLINK_STREAM_BACKPRESSURE_TIMEOUT_MS  5000U

typedef struct {
    xiaozhi_foundation_response_event_kind_t kind;
    uint32_t generation;
    size_t data_len;
    esp_err_t error;
    uint8_t data[DOWNLINK_CHUNK_BYTES];
} downlink_item_t;

static const char *const TAG = "VOICE_DOWNLINK";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue = NULL;
/* The queue is used exclusively from task callbacks/tasks, never from an ISR.
 * Keep its control block in internal RAM while placing its large copied-packet
 * ring in PSRAM so microphone/DMA allocations retain internal-RAM headroom. */
static StaticQueue_t s_queue_control = {0};
static uint8_t *s_queue_storage = NULL;
static TaskHandle_t s_task = NULL;
/* ESP-Xiaozhi invokes the response callbacks synchronously from the 4 KiB
 * WebSocket task. Keep the 2 KiB queue staging item out of that task's stack.
 * The atomic flag rejects unexpected concurrent callback entry rather than
 * allowing two producers to overwrite the shared staging item. xQueueSend()
 * copies the complete item before the flag is released. */
static downlink_item_t s_callback_item = {0};
static atomic_flag s_callback_busy = ATOMIC_FLAG_INIT;
static atomic_bool s_callback_stack_reported = false;
/* The downlink task is the sole owner after initialization. */
static int16_t s_decoded_pcm[VOICE_ASSISTANT_OPUS_PCM_SAMPLES] = {0};
static TickType_t s_last_response_activity = 0U;
static TickType_t s_response_started_at = 0U;
static atomic_bool s_response_tainted = false;
static voice_assistant_downlink_status_t s_status = {0};

static void downlink_set_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.last_error = (error == ESP_OK) ? ESP_FAIL : error;
    ++s_status.responses_failed;
    portEXIT_CRITICAL(&s_lock);
}

static void downlink_mark_response_tainted(
    const char *reason,
    uint32_t generation,
    size_t data_len)
{
    const bool already_tainted = atomic_exchange_explicit(
        &s_response_tainted,
        true,
        memory_order_acq_rel);
    if (!already_tainted) {
        ESP_LOGW(TAG,
                 "response tainted generation=%u reason=%s packet_bytes=%u",
                 (unsigned)generation,
                 reason,
                 (unsigned)data_len);
    }
}

static void downlink_finish_turn_state(void)
{
    atomic_store_explicit(&s_response_tainted, false, memory_order_release);
    portENTER_CRITICAL(&s_lock);
    s_last_response_activity = 0U;
    s_response_started_at = 0U;
    s_status.awaiting_response = false;
    s_status.collecting = false;
    s_status.finalizing = false;
    s_status.playback_requested = false;
    s_status.response_bytes_buffered = 0U;
    portEXIT_CRITICAL(&s_lock);
}

static bool downlink_generation_is_current(uint32_t generation)
{
    if (generation == 0U) {
        return false;
    }
    xiaozhi_foundation_session_status_t session = {0};
    return (xiaozhi_foundation_session_get_status(&session) == ESP_OK) &&
           session.active &&
           (session.state == XIAOZHI_FOUNDATION_SESSION_READY) &&
           (session.client_generation == generation);
}

static void downlink_abort_response(
    uint32_t generation,
    esp_err_t error,
    bool timeout);

/* The PCM stream core uses all-or-nothing writes. Retrying the same decoded
 * packet is therefore safe: a timeout never transfers a partial packet. This
 * runs only in the downlink worker, so s_decoded_pcm stays stable throughout
 * the bounded retry window. */
static esp_err_t downlink_write_pcm_with_backpressure(
    uint32_t generation,
    const int16_t *samples,
    size_t sample_count)
{
    const TickType_t started_at = xTaskGetTickCount();
    bool backpressured = false;

    for (;;) {
        if (atomic_load_explicit(&s_response_tainted,
                                 memory_order_acquire)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const esp_err_t result = phase16_xiaozhi_stream_write(
            samples,
            sample_count);
        /* A callback can mark the response tainted while the arbiter write
         * takes its lock. Do not accept a successful retry after a dropped or
         * oversized Opus packet: terminate the incomplete response instead. */
        if (atomic_load_explicit(&s_response_tainted,
                                 memory_order_acquire)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (result != ESP_ERR_TIMEOUT) {
            if (backpressured && (result == ESP_OK)) {
                ESP_LOGI(TAG,
                         "PCM ingress resumed generation=%u waited_ms=%u",
                         (unsigned)generation,
                         (unsigned)pdTICKS_TO_MS(
                             xTaskGetTickCount() - started_at));
            }
            return result;
        }

        const TickType_t elapsed = xTaskGetTickCount() - started_at;
        if (!backpressured) {
            ESP_LOGW(TAG,
                     "PCM ingress backpressure generation=%u samples=%u; retrying",
                     (unsigned)generation,
                     (unsigned)sample_count);
            backpressured = true;
        }

        if (atomic_load_explicit(&s_response_tainted,
                                 memory_order_acquire)) {
            ESP_LOGW(TAG,
                     "PCM ingress stopped generation=%u: response tainted while backpressured",
                     (unsigned)generation);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (elapsed >= pdMS_TO_TICKS(
                           DOWNLINK_STREAM_BACKPRESSURE_TIMEOUT_MS)) {
            ESP_LOGE(TAG,
                     "PCM ingress backpressure timeout generation=%u waited_ms=%u",
                     (unsigned)generation,
                     (unsigned)pdTICKS_TO_MS(elapsed));
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(pdMS_TO_TICKS(DOWNLINK_STREAM_BACKPRESSURE_RETRY_MS));
    }
}

static void downlink_check_stream_terminal(void)
{
    bool finalizing = false;
    uint32_t generation = 0U;
    portENTER_CRITICAL(&s_lock);
    finalizing = s_status.finalizing;
    generation = s_status.session_generation;
    portEXIT_CRITICAL(&s_lock);
    if (!finalizing || (generation == 0U)) {
        return;
    }

    audio_manager_playback_request_status_t stream = {0};
    const esp_err_t status_result = phase16_xiaozhi_stream_get_status(&stream);
    if (status_result != ESP_OK) {
        if (status_result != ESP_ERR_TIMEOUT) {
            downlink_abort_response(generation, status_result, false);
        }
        return;
    }

    if (stream.state == AUDIO_MANAGER_PLAYBACK_REQUEST_COMPLETED) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.responses_completed;
        s_status.last_error = ESP_OK;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG,
                 "response PLAYBACK_COMPLETE generation=%u accepted=%llu played=%llu high_water=%u starvation=%u",
                 (unsigned)generation,
                 (unsigned long long)stream.pcm_samples_accepted,
                 (unsigned long long)stream.pcm_samples_played,
                 (unsigned)stream.ingress_queue_high_water,
                 (unsigned)stream.starvation_count);
        phase16_xiaozhi_stream_release();
        downlink_finish_turn_state();
    } else if ((stream.state == AUDIO_MANAGER_PLAYBACK_REQUEST_CANCELLED) ||
               (stream.state == AUDIO_MANAGER_PLAYBACK_REQUEST_PREEMPTED) ||
               (stream.state == AUDIO_MANAGER_PLAYBACK_REQUEST_FAILED)) {
        const esp_err_t error = (stream.result == ESP_OK)
            ? ESP_ERR_INVALID_STATE
            : stream.result;
        ESP_LOGW(TAG,
                 "response stream terminal failure generation=%u state=%d error=%s",
                 (unsigned)generation,
                 (int)stream.state,
                 esp_err_to_name(error));
        downlink_abort_response(generation, error, false);
    }
}

static void downlink_response_callback(
    const xiaozhi_foundation_response_event_t *event,
    void *user_context)
{
    (void)user_context;
    if ((event == NULL) || (s_queue == NULL)) {
        return;
    }

    if (atomic_flag_test_and_set_explicit(
            &s_callback_busy,
            memory_order_acquire)) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.chunks_dropped_queue_full;
        portEXIT_CRITICAL(&s_lock);
        downlink_mark_response_tainted(
            "callback-reentry",
            event->client_generation,
            event->data_len);
        return;
    }

    if (event->kind != XIAOZHI_FOUNDATION_RESPONSE_AUDIO) {
        s_callback_item.kind = event->kind;
        s_callback_item.generation = event->client_generation;
        s_callback_item.data_len = 0U;
        s_callback_item.error = event->error;
        if (xQueueSend(s_queue, &s_callback_item, 0U) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            ++s_status.chunks_dropped_queue_full;
            portEXIT_CRITICAL(&s_lock);
            downlink_mark_response_tainted(
                "control-queue-full",
                event->client_generation,
                0U);
        }
        if ((event->kind == XIAOZHI_FOUNDATION_RESPONSE_TTS_START) &&
            !atomic_exchange_explicit(
                &s_callback_stack_reported,
                true,
                memory_order_acq_rel)) {
            ESP_LOGI(TAG,
                     "WebSocket callback stack_hwm=%u staging=static",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        atomic_flag_clear_explicit(&s_callback_busy, memory_order_release);
        return;
    }

    if ((event->data == NULL) || (event->data_len == 0U)) {
        atomic_flag_clear_explicit(&s_callback_busy, memory_order_release);
        return;
    }

    /* A WebSocket binary message is one complete Opus packet. Never split an
     * oversized packet because doing so would destroy the decoder boundary. */
    if (event->data_len > DOWNLINK_CHUNK_BYTES) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.chunks_dropped_queue_full;
        portEXIT_CRITICAL(&s_lock);
        downlink_mark_response_tainted(
            "packet-oversize",
            event->client_generation,
            event->data_len);
        ESP_LOGE(TAG,
                 "Opus packet dropped: size=%u exceeds capacity=%u",
                 (unsigned)event->data_len,
                 (unsigned)DOWNLINK_CHUNK_BYTES);
        atomic_flag_clear_explicit(&s_callback_busy, memory_order_release);
        return;
    }

    s_callback_item.kind = XIAOZHI_FOUNDATION_RESPONSE_AUDIO;
    s_callback_item.generation = event->client_generation;
    s_callback_item.data_len = event->data_len;
    s_callback_item.error = ESP_OK;
    memcpy(s_callback_item.data, event->data, event->data_len);
    if (xQueueSend(s_queue, &s_callback_item, 0U) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.chunks_dropped_queue_full;
        portEXIT_CRITICAL(&s_lock);
        downlink_mark_response_tainted(
            "audio-queue-full",
            event->client_generation,
            event->data_len);
        atomic_flag_clear_explicit(&s_callback_busy, memory_order_release);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    ++s_status.chunks_queued;
    s_status.response_bytes_received += event->data_len;
    portEXIT_CRITICAL(&s_lock);
    atomic_flag_clear_explicit(&s_callback_busy, memory_order_release);
}

static void downlink_abort_response(
    uint32_t generation,
    esp_err_t error,
    bool timeout)
{
    const esp_err_t normalized_error =
        (error == ESP_OK) ? ESP_FAIL : error;
    /* Once a response is aborting, reject any queued audio until the arbiter
     * has accepted the terminal intent. This prevents stale PCM from being
     * appended if the first bounded cleanup attempt meets lock contention. */
    atomic_store_explicit(&s_response_tainted, true, memory_order_release);
    const esp_err_t stream_fail_ret =
        phase16_xiaozhi_stream_fail(normalized_error);
    if ((stream_fail_ret != ESP_OK) &&
        (stream_fail_ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(TAG,
                 "response cleanup pending generation=%u error=%s",
                 (unsigned)generation,
                 esp_err_to_name(stream_fail_ret));
        return;
    }
    (void)xiaozhi_foundation_audio_channel_close(generation);
    if (timeout) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.response_timeouts;
        portEXIT_CRITICAL(&s_lock);
    }
    downlink_set_error(normalized_error);
    ESP_LOGE(TAG,
             "response ABORT generation=%u timeout=%s error=%s",
             (unsigned)generation,
             timeout ? "yes" : "no",
             esp_err_to_name(normalized_error));
    downlink_finish_turn_state();
    if (s_queue != NULL) {
        (void)xQueueReset(s_queue);
    }
}

static void downlink_check_timeout(void)
{
    bool awaiting_response = false;
    bool collecting = false;
    bool finalizing = false;
    uint32_t generation = 0U;
    TickType_t response_started_at = 0U;
    TickType_t last_activity = 0U;
    portENTER_CRITICAL(&s_lock);
    awaiting_response = s_status.awaiting_response;
    collecting = s_status.collecting;
    finalizing = s_status.finalizing;
    generation = s_status.session_generation;
    response_started_at = s_response_started_at;
    last_activity = s_last_response_activity;
    portEXIT_CRITICAL(&s_lock);
    if (!awaiting_response && !collecting && !finalizing) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (finalizing &&
        ((now - last_activity) >=
         pdMS_TO_TICKS(DOWNLINK_STREAM_DRAIN_TIMEOUT_MS))) {
        ESP_LOGE(TAG,
                 "PCM stream drain timeout generation=%u elapsed_ms=%u",
                 (unsigned)generation,
                 (unsigned)pdTICKS_TO_MS(now - last_activity));
        downlink_abort_response(generation, ESP_ERR_TIMEOUT, true);
    } else if ((now - response_started_at) >=
        pdMS_TO_TICKS(DOWNLINK_RESPONSE_MAX_DURATION_MS)) {
        ESP_LOGE(TAG,
                 "response deadline exceeded generation=%u awaiting=%s elapsed_ms=%u",
                 (unsigned)generation,
                 awaiting_response ? "yes" : "no",
                 (unsigned)pdTICKS_TO_MS(now - response_started_at));
        downlink_abort_response(generation, ESP_ERR_TIMEOUT, true);
    } else if ((now - last_activity) >=
               pdMS_TO_TICKS(DOWNLINK_RESPONSE_TIMEOUT_MS)) {
        downlink_abort_response(generation, ESP_ERR_TIMEOUT, true);
    }
}

static void downlink_task(void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "coordinator started queue=%u frames timeout=%ums",
             (unsigned)DOWNLINK_QUEUE_LENGTH,
             (unsigned)DOWNLINK_RESPONSE_TIMEOUT_MS);

    for (;;) {
        /* Check before every dequeue, not only after an empty poll. This keeps
         * the collection deadline effective even if the server repeatedly
         * emits non-terminal events. */
        downlink_check_timeout();
        downlink_check_stream_terminal();

        downlink_item_t item = {0};
        if (xQueueReceive(
                s_queue,
                &item,
                pdMS_TO_TICKS(DOWNLINK_QUEUE_POLL_MS)) != pdTRUE) {
            continue;
        }

        /* A transport-loss error is emitted after foundation has already
         * changed its session state to CONNECTING. It belongs to the in-flight
         * response even though downlink_generation_is_current() is therefore
         * false. Conversely, a server CHAT_ERROR can arrive while READY before
         * TTS_START; it must still close the open audio channel. Consume both
         * cases before the normal READY-only generation gate. */
        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_ERROR) {
            bool owns_response = false;
            portENTER_CRITICAL(&s_lock);
            owns_response = (s_status.awaiting_response || s_status.collecting ||
                             s_status.finalizing || s_status.playback_requested) &&
                            (s_status.session_generation == item.generation);
            portEXIT_CRITICAL(&s_lock);
            if (owns_response) {
                const esp_err_t error =
                    (item.error == ESP_OK) ? ESP_FAIL : item.error;
                ESP_LOGW(TAG,
                         "response error generation=%u error=%s; aborting collection",
                         (unsigned)item.generation,
                         esp_err_to_name(error));
                downlink_abort_response(item.generation, error, false);
            } else {
                portENTER_CRITICAL(&s_lock);
                ++s_status.chunks_dropped_stale;
                portEXIT_CRITICAL(&s_lock);
            }
            continue;
        }

        if (!downlink_generation_is_current(item.generation)) {
            portENTER_CRITICAL(&s_lock);
            ++s_status.chunks_dropped_stale;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_TTS_START) {
            const TickType_t started_at = xTaskGetTickCount();
            bool accept = false;
            portENTER_CRITICAL(&s_lock);
            accept = s_status.awaiting_response &&
                     !s_status.collecting &&
                     !s_status.finalizing &&
                     !s_status.playback_requested &&
                     (s_status.session_generation == item.generation);
            if (accept) {
                /* Claim the response before resetting the decoder. A failed
                 * stop-listening cancellation now sees awaiting_response=false
                 * and leaves this queued TTS_START under downlink ownership. */
                s_response_started_at = started_at;
                s_last_response_activity = started_at;
                s_status.awaiting_response = false;
                s_status.collecting = true;
                s_status.finalizing = false;
                s_status.playback_requested = false;
                s_status.response_bytes_buffered = 0U;
                s_status.last_error = ESP_OK;
            }
            portEXIT_CRITICAL(&s_lock);
            if (!accept) {
                portENTER_CRITICAL(&s_lock);
                ++s_status.chunks_dropped_stale;
                portEXIT_CRITICAL(&s_lock);
                continue;
            }
            atomic_store_explicit(&s_response_tainted, false, memory_order_release);
            const esp_err_t reset_ret = voice_assistant_opus_decoder_reset();
            if (reset_ret != ESP_OK) {
                downlink_abort_response(item.generation, reset_ret, false);
                continue;
            }
            const esp_err_t stream_ret = phase16_xiaozhi_stream_begin();
            if (stream_ret != ESP_OK) {
                downlink_abort_response(item.generation, stream_ret, false);
                continue;
            }
            ESP_LOGI(TAG,
                     "response START generation=%u mode=pcm16_stream",
                     (unsigned)item.generation);
            continue;
        }

        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_AUDIO) {
            bool accept = false;
            portENTER_CRITICAL(&s_lock);
            accept = s_status.collecting &&
                     (s_status.session_generation == item.generation);
            portEXIT_CRITICAL(&s_lock);
            if (!accept) {
                portENTER_CRITICAL(&s_lock);
                ++s_status.chunks_dropped_stale;
                portEXIT_CRITICAL(&s_lock);
                continue;
            }
            size_t decoded_samples = 0U;
            const esp_err_t decode_ret = voice_assistant_opus_decode(
                item.data,
                item.data_len,
                s_decoded_pcm,
                VOICE_ASSISTANT_OPUS_PCM_SAMPLES,
                &decoded_samples);
            if (decode_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "Opus decode rejected generation=%u packet_bytes=%u error=%s",
                         (unsigned)item.generation,
                         (unsigned)item.data_len,
                         esp_err_to_name(decode_ret));
                downlink_abort_response(item.generation, decode_ret, false);
                continue;
            }
            const size_t decoded_bytes = decoded_samples * sizeof(int16_t);
            bool first_packet = false;
            portENTER_CRITICAL(&s_lock);
            first_packet = (s_status.response_bytes_buffered == 0U);
            portEXIT_CRITICAL(&s_lock);
            if (first_packet) {
                ESP_LOGI(TAG,
                         "first Opus packet decoded generation=%u opus_bytes=%u pcm_bytes=%u stack_hwm=%u",
                         (unsigned)item.generation,
                         (unsigned)item.data_len,
                         (unsigned)decoded_bytes,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            const esp_err_t stream_ret = downlink_write_pcm_with_backpressure(
                item.generation,
                s_decoded_pcm,
                decoded_samples);
            if (stream_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "PCM stream rejected generation=%u samples=%u error=%s",
                         (unsigned)item.generation,
                         (unsigned)decoded_samples,
                         esp_err_to_name(stream_ret));
                downlink_abort_response(item.generation, stream_ret, false);
                continue;
            }
            portENTER_CRITICAL(&s_lock);
            s_last_response_activity = xTaskGetTickCount();
            s_status.response_bytes_buffered += decoded_bytes;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_TTS_STOP) {
            bool accept = false;
            portENTER_CRITICAL(&s_lock);
            accept = s_status.collecting &&
                     (s_status.session_generation == item.generation);
            if (accept) {
                s_status.collecting = false;
                s_status.finalizing = true;
                s_status.playback_requested = true;
                s_last_response_activity = xTaskGetTickCount();
            }
            portEXIT_CRITICAL(&s_lock);
            if (!accept) {
                portENTER_CRITICAL(&s_lock);
                ++s_status.chunks_dropped_stale;
                portEXIT_CRITICAL(&s_lock);
                continue;
            }
            const bool tainted = atomic_load_explicit(
                &s_response_tainted,
                memory_order_acquire);
            uint64_t pcm_bytes = 0U;
            portENTER_CRITICAL(&s_lock);
            pcm_bytes = s_status.response_bytes_buffered;
            portEXIT_CRITICAL(&s_lock);
            if (tainted || (pcm_bytes == 0U)) {
                downlink_abort_response(
                    item.generation,
                    tainted ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_INVALID_SIZE,
                    false);
                continue;
            }
            const esp_err_t finish_ret = phase16_xiaozhi_stream_finish();
            if (finish_ret != ESP_OK) {
                downlink_abort_response(item.generation, finish_ret, false);
                continue;
            }
            const esp_err_t close_ret =
                xiaozhi_foundation_audio_channel_close(item.generation);
            if ((close_ret != ESP_OK) && (close_ret != ESP_ERR_INVALID_STATE)) {
                downlink_abort_response(item.generation, close_ret, false);
                continue;
            }
            ESP_LOGI(TAG,
                     "response DRAINING generation=%u accepted_pcm_bytes=%llu",
                     (unsigned)item.generation,
                     (unsigned long long)pcm_bytes);
        }
    }
}

esp_err_t voice_assistant_downlink_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_status.initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);

    const esp_err_t codec_ret = voice_assistant_opus_decoder_init();
    if (codec_ret != ESP_OK) {
        return codec_ret;
    }

    const size_t queue_storage_bytes =
        (size_t)DOWNLINK_QUEUE_LENGTH * sizeof(downlink_item_t);
    s_queue_storage = (uint8_t *)heap_caps_malloc(
        queue_storage_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue_storage == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreateStatic(
        DOWNLINK_QUEUE_LENGTH,
        sizeof(downlink_item_t),
        s_queue_storage,
        &s_queue_control);
    if (s_queue == NULL) {
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    atomic_store_explicit(&s_response_tainted, false, memory_order_release);
    portENTER_CRITICAL(&s_lock);
    s_last_response_activity = 0U;
    s_response_started_at = 0U;
    s_status = (voice_assistant_downlink_status_t) {
        .initialized = true,
        .last_error = ESP_OK,
    };
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_downlink_start(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool initialized = s_status.initialized;
    const bool running = s_status.running || (s_task != NULL);
    portEXIT_CRITICAL(&s_lock);
    if (!initialized || (s_queue == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (running) {
        return ESP_OK;
    }

    esp_err_t ret = xiaozhi_foundation_response_register_callback(
        downlink_response_callback, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xTaskCreate(
            downlink_task,
            DOWNLINK_TASK_NAME,
            DOWNLINK_TASK_STACK_BYTES,
            NULL,
            DOWNLINK_TASK_PRIORITY,
            &s_task) != pdPASS) {
        (void)xiaozhi_foundation_response_register_callback(NULL, NULL);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_assistant_downlink_begin_response_wait(uint32_t session_generation)
{
    if (session_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const TickType_t started_at = xTaskGetTickCount();
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_lock);
    if (!s_status.initialized || !s_status.running ||
        s_status.awaiting_response || s_status.collecting ||
        s_status.finalizing || s_status.playback_requested) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_response_started_at = started_at;
        s_last_response_activity = started_at;
        s_status.awaiting_response = true;
        s_status.session_generation = session_generation;
        s_status.last_error = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "response WAIT generation=%u", (unsigned)session_generation);
    }
    return ret;
}

esp_err_t voice_assistant_downlink_cancel_response_wait(
    uint32_t session_generation,
    esp_err_t error)
{
    if (session_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    bool cancelled = false;
    portENTER_CRITICAL(&s_lock);
    if (s_status.awaiting_response &&
        (s_status.session_generation == session_generation)) {
        s_response_started_at = 0U;
        s_last_response_activity = 0U;
        s_status.awaiting_response = false;
        s_status.last_error = (error == ESP_OK) ? ESP_FAIL : error;
        cancelled = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (!cancelled) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG,
             "response WAIT cancelled generation=%u error=%s",
             (unsigned)session_generation,
             esp_err_to_name((error == ESP_OK) ? ESP_FAIL : error));
    return ESP_OK;
}

esp_err_t voice_assistant_downlink_get_status(
    voice_assistant_downlink_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool voice_assistant_downlink_is_busy(void)
{
    bool busy = false;
    portENTER_CRITICAL(&s_lock);
    busy = s_status.awaiting_response || s_status.collecting ||
           s_status.finalizing || s_status.playback_requested;
    portEXIT_CRITICAL(&s_lock);
    return busy;
}
