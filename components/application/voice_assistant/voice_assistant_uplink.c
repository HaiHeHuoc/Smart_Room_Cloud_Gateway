#include "voice_assistant_uplink.h"

#include <string.h>

#include "audio_manager.h"
#include "audio_manager_stream.h"
#include "voice_assistant_downlink.h"
#include "voice_assistant_opus.h"
#include "voice_assistant_ptt.h"
#include "xiaozhi_foundation.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define UPLINK_TASK_NAME             "voice_uplink"
/* esp_audio_codec documents about 40 KiB of task stack for encoder coverage.
 * Opus processing must not run on the former 5 KiB coordinator stack. */
#define UPLINK_TASK_STACK_BYTES      (40U * 1024U)
#define UPLINK_TASK_PRIORITY         5U
#define UPLINK_QUEUE_LENGTH          8U
#define UPLINK_RECONCILE_MS          20U

typedef struct {
    uint32_t generation;
    uint64_t sequence;
    size_t sample_count;
    int16_t samples[AUDIO_MANAGER_STREAM_FRAME_SAMPLES];
} uplink_frame_item_t;

static const char *const TAG = "VOICE_UPLINK";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static voice_assistant_uplink_status_t s_status = {0};
/* Owned exclusively by the uplink task after initialization. */
static int16_t s_pcm_frame[VOICE_ASSISTANT_OPUS_PCM_SAMPLES] = {0};
static size_t s_pcm_frame_samples = 0U;
static uint8_t s_opus_packet[VOICE_ASSISTANT_OPUS_MAX_PACKET_BYTES] = {0};
static uint32_t s_turn_packets = 0U;
static uint32_t s_turn_opus_bytes = 0U;
static uint32_t s_turn_pcm_samples = 0U;

static void uplink_set_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_lock);
}

static void uplink_stream_callback(
    const audio_manager_stream_frame_t *frame,
    void *user_context)
{
    (void)user_context;
    if ((frame == NULL) || (frame->samples == NULL) ||
        (frame->sample_count == 0U) ||
        (frame->sample_count > AUDIO_MANAGER_STREAM_FRAME_SAMPLES) ||
        (s_queue == NULL)) {
        return;
    }

    bool accept = false;
    portENTER_CRITICAL(&s_lock);
    accept = s_status.turn_active &&
             (s_status.session_generation == frame->stream_generation);
    portEXIT_CRITICAL(&s_lock);
    if (!accept) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.frames_dropped_stale;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    uplink_frame_item_t item = {
        .generation = frame->stream_generation,
        .sequence = frame->frame_sequence,
        .sample_count = frame->sample_count,
    };
    memcpy(item.samples,
           frame->samples,
           frame->sample_count * sizeof(item.samples[0]));

    if (xQueueSend(s_queue, &item, 0U) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.frames_dropped_queue_full;
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    portENTER_CRITICAL(&s_lock);
    ++s_status.frames_queued;
    portEXIT_CRITICAL(&s_lock);
}

static bool uplink_ptt_authorizes(uint32_t generation)
{
    voice_assistant_ptt_status_t ptt = {0};
    return (voice_assistant_ptt_get_status(&ptt) == ESP_OK) &&
           (ptt.state == VOICE_ASSISTANT_PTT_AUTHORIZED) &&
           ptt.capture_authorized &&
           (ptt.session_generation == generation);
}

static esp_err_t uplink_begin_turn(uint32_t generation)
{
    if (generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (voice_assistant_downlink_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = voice_assistant_opus_encoder_reset();
    if (ret != ESP_OK) {
        return ret;
    }

    audio_manager_status_t audio = {0};
    ret = audio_manager_get_status(&audio);
    if ((ret != ESP_OK) || (audio.state != AUDIO_MANAGER_STATE_IDLE)) {
        return (ret != ESP_OK) ? ret : ESP_ERR_INVALID_STATE;
    }

    ret = xiaozhi_foundation_audio_uplink_start(generation);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Opening the remote audio channel can block for several seconds. The
     * user may release PTT during that wait, so revalidate authorization
     * before arming I2S capture. */
    if (!uplink_ptt_authorizes(generation)) {
        (void)xiaozhi_foundation_audio_uplink_stop(generation);
        (void)xiaozhi_foundation_audio_channel_close(generation);
        ESP_LOGI(TAG,
                 "turn CANCELLED before capture generation=%u",
                 (unsigned)generation);
        return ESP_ERR_INVALID_STATE;
    }

    ret = audio_manager_stream_arm(generation);
    if (ret != ESP_OK) {
        (void)xiaozhi_foundation_audio_uplink_stop(generation);
        (void)xiaozhi_foundation_audio_channel_close(generation);
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.turn_active = true;
    s_status.session_generation = generation;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    s_pcm_frame_samples = 0U;
    s_turn_packets = 0U;
    s_turn_opus_bytes = 0U;
    s_turn_pcm_samples = 0U;

    ret = audio_manager_start_recording();
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.turn_active = false;
        portEXIT_CRITICAL(&s_lock);
        (void)audio_manager_stream_disarm(generation);
        (void)xiaozhi_foundation_audio_uplink_stop(generation);
        (void)xiaozhi_foundation_audio_channel_close(generation);
        return ret;
    }

    ESP_LOGI(TAG, "turn START generation=%u", (unsigned)generation);
    return ESP_OK;
}

static esp_err_t uplink_end_turn(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    s_status.turn_active = false;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t first_error = ESP_OK;
    bool response_wait_started = false;
    bool close_after_stop = (s_turn_packets == 0U);
    esp_err_t ret = audio_manager_stream_disarm(generation);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        first_error = ret;
    }

    /* Reserve the still-open shared channel before stop-listening reaches the
     * server. This closes the former gap where a second PTT press could be
     * authorized while the prior turn had sent audio but had not yet received
     * TTS_START. */
    if (s_turn_packets > 0U) {
        ret = voice_assistant_downlink_begin_response_wait(generation);
        if (ret == ESP_OK) {
            response_wait_started = true;
        } else {
            /* Never retain an untracked audio channel: if downlink cannot
             * own the response wait, close it after stop-listening instead. */
            close_after_stop = true;
            if (first_error == ESP_OK) {
                first_error = ret;
            }
        }
    }

    ret = audio_manager_stop_recording();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE) &&
        (first_error == ESP_OK)) {
        first_error = ret;
    }

    /* Stop listening and retain the channel only while downlink owns the
     * bounded response wait. A zero-packet turn cannot produce a valid
     * response and must not block the next PTT attempt. */
    ret = xiaozhi_foundation_audio_uplink_stop(generation);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE) &&
        (first_error == ESP_OK)) {
        first_error = ret;
    }

    if ((ret != ESP_OK) && response_wait_started) {
        /* No successful stop-listening means the server cannot be relied on
         * to produce a response. Clear the local wait before closing the
         * channel; if TTS_START won the race, cancellation returns
         * INVALID_STATE and the downlink worker remains its owner. */
        if (voice_assistant_downlink_cancel_response_wait(generation, ret) ==
            ESP_OK) {
            close_after_stop = true;
        }
    }

    if (close_after_stop) {
        ret = xiaozhi_foundation_audio_channel_close(generation);
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE) &&
            (first_error == ESP_OK)) {
            first_error = ret;
        }
    }

    (void)xQueueReset(s_queue);
    s_pcm_frame_samples = 0U;
    ESP_LOGI(TAG,
             "turn STOP generation=%u result=%s opus_packets=%u opus_bytes=%u pcm_samples=%u channel_retained=%s",
             (unsigned)generation,
             esp_err_to_name(first_error),
             (unsigned)s_turn_packets,
             (unsigned)s_turn_opus_bytes,
             (unsigned)s_turn_pcm_samples,
             (response_wait_started && !close_after_stop) ? "yes" : "no");
    return first_error;
}

static esp_err_t uplink_encode_and_send(
    uint32_t generation,
    const int16_t *samples,
    size_t sample_count)
{
    size_t packet_size = 0U;
    esp_err_t ret = voice_assistant_opus_encode(
        samples,
        sample_count,
        s_opus_packet,
        sizeof(s_opus_packet),
        &packet_size);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = xiaozhi_foundation_audio_uplink_send_opus_packet(
        generation, s_opus_packet, packet_size);
    if (ret == ESP_OK) {
        ++s_turn_packets;
        s_turn_opus_bytes += packet_size;
        s_turn_pcm_samples += sample_count;
        portENTER_CRITICAL(&s_lock);
        ++s_status.frames_sent;
        portEXIT_CRITICAL(&s_lock);
        if (s_turn_packets == 1U) {
            ESP_LOGI(TAG,
                     "first Opus packet generation=%u bytes=%u stack_hwm=%u",
                     (unsigned)generation,
                     (unsigned)packet_size,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
    return ret;
}

static esp_err_t uplink_consume_pcm(const uplink_frame_item_t *item)
{
    size_t offset = 0U;
    while (offset < item->sample_count) {
        size_t copy_samples = item->sample_count - offset;
        const size_t available =
            VOICE_ASSISTANT_OPUS_PCM_SAMPLES - s_pcm_frame_samples;
        if (copy_samples > available) {
            copy_samples = available;
        }
        memcpy(&s_pcm_frame[s_pcm_frame_samples],
               &item->samples[offset],
               copy_samples * sizeof(s_pcm_frame[0]));
        s_pcm_frame_samples += copy_samples;
        offset += copy_samples;

        if (s_pcm_frame_samples == VOICE_ASSISTANT_OPUS_PCM_SAMPLES) {
            const esp_err_t ret = uplink_encode_and_send(
                item->generation,
                s_pcm_frame,
                s_pcm_frame_samples);
            s_pcm_frame_samples = 0U;
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }
    return ESP_OK;
}

static void uplink_reconcile_ptt(void)
{
    voice_assistant_ptt_status_t ptt = {0};
    if (voice_assistant_ptt_get_status(&ptt) != ESP_OK) {
        return;
    }

    bool active = false;
    uint32_t generation = 0U;
    portENTER_CRITICAL(&s_lock);
    active = s_status.turn_active;
    generation = s_status.session_generation;
    portEXIT_CRITICAL(&s_lock);

    if (!active &&
        (ptt.state == VOICE_ASSISTANT_PTT_AUTHORIZED) &&
        ptt.capture_authorized &&
        (ptt.session_generation != 0U)) {
        if (voice_assistant_downlink_is_busy()) {
            return;
        }
        const esp_err_t ret = uplink_begin_turn(ptt.session_generation);
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
            ESP_LOGE(TAG, "turn start failed: %s", esp_err_to_name(ret));
            uplink_set_error(ret);
        }
        return;
    }

    if (active &&
        ((ptt.state != VOICE_ASSISTANT_PTT_AUTHORIZED) ||
         !ptt.capture_authorized ||
         (ptt.session_generation != generation))) {
        const esp_err_t ret = uplink_end_turn(generation);
        if (ret != ESP_OK) {
            uplink_set_error(ret);
        }
    }
}

static void uplink_task(void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "coordinator started");

    for (;;) {
        uplink_frame_item_t item = {0};
        if (xQueueReceive(
                s_queue,
                &item,
                pdMS_TO_TICKS(UPLINK_RECONCILE_MS)) == pdTRUE) {
            bool current = false;
            portENTER_CRITICAL(&s_lock);
            current = s_status.turn_active &&
                      (s_status.session_generation == item.generation);
            portEXIT_CRITICAL(&s_lock);

            if (!current) {
                portENTER_CRITICAL(&s_lock);
                ++s_status.frames_dropped_stale;
                portEXIT_CRITICAL(&s_lock);
            } else {
                const esp_err_t ret = uplink_consume_pcm(&item);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG,
                             "Opus TX failed generation=%u seq=%llu: %s",
                             (unsigned)item.generation,
                             (unsigned long long)item.sequence,
                             esp_err_to_name(ret));
                    uplink_set_error(ret);
                    (void)voice_assistant_ptt_cancel();
                }
            }
        }

        uplink_reconcile_ptt();
    }
}

esp_err_t voice_assistant_uplink_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }

    s_queue = xQueueCreate(UPLINK_QUEUE_LENGTH, sizeof(uplink_frame_item_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.last_error = ESP_OK;

    const esp_err_t codec_ret = voice_assistant_opus_encoder_init();
    if (codec_ret != ESP_OK) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return codec_ret;
    }

    const esp_err_t ret = audio_manager_stream_register_callback(
        uplink_stream_callback, NULL);
    if (ret != ESP_OK) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ret;
    }
    return ESP_OK;
}

esp_err_t voice_assistant_uplink_start(void)
{
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(
            uplink_task,
            UPLINK_TASK_NAME,
            UPLINK_TASK_STACK_BYTES,
            NULL,
            UPLINK_TASK_PRIORITY,
            &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_assistant_uplink_get_status(
    voice_assistant_uplink_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
