#include "voice_assistant_downlink.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "audio_manager.h"
#include "sd_card_manager.h"
#include "voice_assistant_opus.h"
#include "xiaozhi_foundation.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DOWNLINK_TASK_NAME                 "voice_downlink"
/* esp_audio_codec documents about 20 KiB of task stack for decoder coverage;
 * keep additional coordinator headroom for queue/WAV orchestration. */
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
/* Keep complete PCM until TTS_STOP so the existing audio_manager WAV handoff
 * remains atomic. Two MiB stores up to about 65 seconds of 16 kHz mono PCM16
 * on the verified 8 MiB-PSRAM target, while leaving multiple MiB free after
 * the microphone and WAV-prefetch allocations. */
#define DOWNLINK_RESPONSE_CAPACITY_BYTES   (2U * 1024U * 1024U)
#define DOWNLINK_RESPONSE_TIMEOUT_MS       15000U
/* A complete 2 MiB PCM response is about 65 seconds at the negotiated
 * format. Bound the entire collection too, so duplicate protocol events or
 * a server that omits TTS_STOP cannot hold the next PTT turn forever. */
#define DOWNLINK_RESPONSE_MAX_DURATION_MS  90000U
#define DOWNLINK_QUEUE_POLL_MS             100U
#define DOWNLINK_AUDIO_IDLE_TIMEOUT_MS     10000U
#define DOWNLINK_PLAYBACK_TIMEOUT_MS       60000U
#define DOWNLINK_AUDIO_IDLE_POLL_MS        50U
#define DOWNLINK_WAV_PATH                  "/sdcard/xiaozhi_response.wav"

_Static_assert((DOWNLINK_RESPONSE_CAPACITY_BYTES % 2U) == 0U,
               "PCM16 response capacity must be sample aligned");

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
static uint8_t *s_response = NULL;
static size_t s_response_size = 0U;
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
    s_response_size = 0U;
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

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8U) & 0xffU);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8U) & 0xffU);
    dst[2] = (uint8_t)((value >> 16U) & 0xffU);
    dst[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static esp_err_t downlink_write_wav(const uint8_t *pcm, size_t pcm_bytes)
{
    if ((pcm == NULL) || (pcm_bytes == 0U) || ((pcm_bytes % 2U) != 0U) ||
        (pcm_bytes > UINT32_MAX - 36U)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sd_card_manager_acquire();
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *file = fopen(DOWNLINK_WAV_PATH, "wb");
    if (file == NULL) {
        const int saved_errno = errno;
        sd_card_manager_release();
        if (sd_card_manager_is_vfs_media_error(saved_errno)) {
            sd_card_manager_report_io_error(ESP_FAIL);
        }
        return ESP_FAIL;
    }

    uint8_t header[44] = {0};
    memcpy(&header[0], "RIFF", 4U);
    put_u32_le(&header[4], (uint32_t)(36U + pcm_bytes));
    memcpy(&header[8], "WAVEfmt ", 8U);
    put_u32_le(&header[16], 16U);
    put_u16_le(&header[20], 1U);
    put_u16_le(&header[22], 1U);
    put_u32_le(&header[24], XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ);
    put_u32_le(&header[28], XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ * 2U);
    put_u16_le(&header[32], 2U);
    put_u16_le(&header[34], 16U);
    memcpy(&header[36], "data", 4U);
    put_u32_le(&header[40], (uint32_t)pcm_bytes);

    bool io_failed = false;
    if (fwrite(header, 1U, sizeof(header), file) != sizeof(header)) {
        io_failed = true;
    }
    if (!io_failed && (fwrite(pcm, 1U, pcm_bytes, file) != pcm_bytes)) {
        io_failed = true;
    }
    if (!io_failed && (fflush(file) != 0)) {
        io_failed = true;
    }
    if (fclose(file) != 0) {
        io_failed = true;
    }
    sd_card_manager_release();

    if (io_failed) {
        sd_card_manager_report_io_error(ESP_FAIL);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t downlink_wait_audio_idle(uint32_t timeout_ms)
{
    uint32_t waited = 0U;
    while (waited <= timeout_ms) {
        audio_manager_status_t audio = {0};
        const esp_err_t ret = audio_manager_get_status(&audio);
        if (ret != ESP_OK) {
            return ret;
        }
        if (audio.state == AUDIO_MANAGER_STATE_IDLE) {
            return ESP_OK;
        }
        if ((audio.state == AUDIO_MANAGER_STATE_ERROR) ||
            (audio.state == AUDIO_MANAGER_STATE_UNINITIALIZED) ||
            (audio.state == AUDIO_MANAGER_STATE_INITIALIZED)) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(DOWNLINK_AUDIO_IDLE_POLL_MS));
        waited += DOWNLINK_AUDIO_IDLE_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t downlink_wait_playback_complete(void)
{
    bool playback_seen = false;
    uint32_t waited = 0U;
    while (waited <= DOWNLINK_PLAYBACK_TIMEOUT_MS) {
        audio_manager_status_t audio = {0};
        const esp_err_t ret = audio_manager_get_status(&audio);
        if (ret != ESP_OK) {
            return ret;
        }
        if (audio.state == AUDIO_MANAGER_STATE_PLAYBACK) {
            playback_seen = true;
        } else if (playback_seen && (audio.state == AUDIO_MANAGER_STATE_IDLE)) {
            return (audio.last_error == ESP_OK) ? ESP_OK : audio.last_error;
        } else if ((audio.state == AUDIO_MANAGER_STATE_ERROR) ||
                   (audio.state == AUDIO_MANAGER_STATE_UNINITIALIZED) ||
                   (audio.state == AUDIO_MANAGER_STATE_INITIALIZED)) {
            return (audio.last_error == ESP_OK) ? ESP_ERR_INVALID_STATE : audio.last_error;
        }
        vTaskDelay(pdMS_TO_TICKS(DOWNLINK_AUDIO_IDLE_POLL_MS));
        waited += DOWNLINK_AUDIO_IDLE_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t downlink_finalize_response(uint32_t generation)
{
    const bool tainted = atomic_load_explicit(
        &s_response_tainted,
        memory_order_acquire);
    if ((generation == 0U) || (s_response_size == 0U) ||
        ((s_response_size % sizeof(int16_t)) != 0U) || tainted) {
        uint32_t queued = 0U;
        uint32_t dropped_queue_full = 0U;
        uint32_t dropped_stale = 0U;
        portENTER_CRITICAL(&s_lock);
        queued = s_status.chunks_queued;
        dropped_queue_full = s_status.chunks_dropped_queue_full;
        dropped_stale = s_status.chunks_dropped_stale;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG,
                 "response rejected generation=%u pcm_bytes=%u tainted=%s queued=%u dropped_queue_full=%u dropped_stale=%u",
                 (unsigned)generation,
                 (unsigned)s_response_size,
                 tainted ? "yes" : "no",
                 (unsigned)queued,
                 (unsigned)dropped_queue_full,
                 (unsigned)dropped_stale);
        return tainted ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_INVALID_SIZE;
    }

    esp_err_t first_error = xiaozhi_foundation_audio_channel_close(generation);
    if ((first_error != ESP_OK) && (first_error != ESP_ERR_INVALID_STATE)) {
        return first_error;
    }

    first_error = downlink_write_wav(s_response, s_response_size);
    if (first_error != ESP_OK) {
        return first_error;
    }

    first_error = downlink_wait_audio_idle(DOWNLINK_AUDIO_IDLE_TIMEOUT_MS);
    if (first_error != ESP_OK) {
        return first_error;
    }

    first_error = audio_manager_play_wav(DOWNLINK_WAV_PATH);
    if (first_error != ESP_OK) {
        return first_error;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.playback_requested = true;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG,
             "response PLAYBACK_REQUESTED generation=%u pcm_bytes=%u path=%s",
             (unsigned)generation,
             (unsigned)s_response_size,
             DOWNLINK_WAV_PATH);

    first_error = downlink_wait_playback_complete();
    if (first_error != ESP_OK) {
        return first_error;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.playback_requested = false;
    ++s_status.responses_completed;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "response PLAYBACK_COMPLETE generation=%u",
             (unsigned)generation);
    return ESP_OK;
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
    (void)xiaozhi_foundation_audio_channel_close(generation);
    if (timeout) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.response_timeouts;
        portEXIT_CRITICAL(&s_lock);
    }
    downlink_set_error(error);
    ESP_LOGE(TAG,
             "response ABORT generation=%u timeout=%s error=%s",
             (unsigned)generation,
             timeout ? "yes" : "no",
             esp_err_to_name(error));
    downlink_finish_turn_state();
    if (s_queue != NULL) {
        (void)xQueueReset(s_queue);
    }
}

static void downlink_check_timeout(void)
{
    bool awaiting_response = false;
    bool collecting = false;
    uint32_t generation = 0U;
    TickType_t response_started_at = 0U;
    TickType_t last_activity = 0U;
    portENTER_CRITICAL(&s_lock);
    awaiting_response = s_status.awaiting_response;
    collecting = s_status.collecting;
    generation = s_status.session_generation;
    response_started_at = s_response_started_at;
    last_activity = s_last_response_activity;
    portEXIT_CRITICAL(&s_lock);
    if (!awaiting_response && !collecting) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if ((now - response_started_at) >=
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
    ESP_LOGI(TAG, "coordinator started capacity=%uB timeout=%ums",
             (unsigned)DOWNLINK_RESPONSE_CAPACITY_BYTES,
             (unsigned)DOWNLINK_RESPONSE_TIMEOUT_MS);

    for (;;) {
        /* Check before every dequeue, not only after an empty poll. This keeps
         * the collection deadline effective even if the server repeatedly
         * emits non-terminal events. */
        downlink_check_timeout();

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
            owns_response = (s_status.awaiting_response || s_status.collecting) &&
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
            s_response_size = 0U;
            atomic_store_explicit(&s_response_tainted, false, memory_order_release);
            const esp_err_t reset_ret = voice_assistant_opus_decoder_reset();
            if (reset_ret != ESP_OK) {
                downlink_abort_response(item.generation, reset_ret, false);
                continue;
            }
            ESP_LOGI(TAG, "response START generation=%u", (unsigned)item.generation);
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
            if (s_response_size == 0U) {
                ESP_LOGI(TAG,
                         "first Opus packet decoded generation=%u opus_bytes=%u pcm_bytes=%u stack_hwm=%u",
                         (unsigned)item.generation,
                         (unsigned)item.data_len,
                         (unsigned)decoded_bytes,
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            if ((s_response_size + decoded_bytes) >
                DOWNLINK_RESPONSE_CAPACITY_BYTES) {
                downlink_abort_response(item.generation, ESP_ERR_NO_MEM, false);
                continue;
            }
            memcpy(&s_response[s_response_size], s_decoded_pcm, decoded_bytes);
            s_response_size += decoded_bytes;
            portENTER_CRITICAL(&s_lock);
            s_last_response_activity = xTaskGetTickCount();
            s_status.response_bytes_buffered = s_response_size;
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
            }
            portEXIT_CRITICAL(&s_lock);
            if (!accept) {
                portENTER_CRITICAL(&s_lock);
                ++s_status.chunks_dropped_stale;
                portEXIT_CRITICAL(&s_lock);
                continue;
            }
            const esp_err_t ret = downlink_finalize_response(item.generation);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "response finalize failed generation=%u error=%s",
                         (unsigned)item.generation,
                         esp_err_to_name(ret));
                downlink_abort_response(item.generation, ret, false);
                continue;
            }
            downlink_finish_turn_state();
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

    s_response = (uint8_t *)heap_caps_malloc(
        DOWNLINK_RESPONSE_CAPACITY_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_response == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_response_size = 0U;
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
    if (!initialized || (s_queue == NULL) || (s_response == NULL)) {
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
