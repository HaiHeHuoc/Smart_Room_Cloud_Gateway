#include "voice_assistant_uplink.h"

#include <string.h>
#include <inttypes.h>

#include "audio_manager.h"
#include "audio_manager_stream.h"
#include "voice_assistant_audio_arbitration_bridge.h"
#include "voice_assistant_downlink.h"
#include "voice_assistant_opus.h"
#include "voice_assistant_playback_control.h"
#include "voice_assistant_ptt.h"
#include "xiaozhi_foundation.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_recording_critical.h"

#define UPLINK_TASK_NAME             "voice_uplink"
/* The target's failed-turn high-water mark left 11 KiB unused at 36 KiB.
 * 32 KiB retains more than 7 KiB stack margin while returning 4 KiB of
 * Internal RAM to the TLS/I2S contention boundary. */
#define UPLINK_TASK_STACK_BYTES      (32U * 1024U)
/* Live uplink must outrank normal priority-5 GUI/Web/effect work while
 * remaining below the priority-7 I2S capture owner. */
#define UPLINK_TASK_PRIORITY         6U
/* 16 x 256 samples at 16 kHz gives ~256 ms of bounded PCM jitter headroom.
 * Storage remains PSRAM-backed; the audio-manager callback still never blocks. */
#define UPLINK_QUEUE_LENGTH          16U
#define UPLINK_RECONCILE_MS          20U
/* mbedTLS owns dynamic WebSocket records in PSRAM. Reserve enough contiguous
 * PSRAM for the 16 KiB receive record, the bounded TX record, and allocator
 * metadata before PTT begins so a low-memory turn is rejected locally rather
 * than failing a WebSocket write after I2S capture has started. */
#define UPLINK_MIN_TLS_PSRAM_HEADROOM_BYTES (20U * 1024U)

typedef struct {
    uint32_t generation;
    uint64_t sequence;
    size_t sample_count;
    int16_t samples[AUDIO_MANAGER_STREAM_FRAME_SAMPLES];
} uplink_frame_item_t;

static const char *const TAG = "VOICE_UPLINK";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue = NULL;
/* Audio-manager calls the stream callback only from task context. Its copied
 * queue payload can therefore live in PSRAM, retaining Internal/DMA heap for
 * I2S descriptors and mbedTLS/AES allocations. */
static StaticQueue_t s_queue_control = {0};
static uint8_t *s_queue_storage = NULL;
static TaskHandle_t s_task = NULL;
static voice_assistant_uplink_status_t s_status = {0};
/* Owned exclusively by the uplink task after initialization. */
static int16_t s_pcm_frame[VOICE_ASSISTANT_OPUS_PCM_SAMPLES] = {0};
static size_t s_pcm_frame_samples = 0U;
static uint8_t s_opus_packet[VOICE_ASSISTANT_OPUS_MAX_PACKET_BYTES] = {0};
static uint32_t s_turn_packets = 0U;
static uint32_t s_turn_opus_bytes = 0U;
static uint32_t s_turn_pcm_samples = 0U;
static uint32_t s_turn_ptt_generation = 0U;
static bool s_turn_critical_active = false;
/* Avoid repeatedly queuing and logging the same terminal cancellation while
 * the PTT state-machine task consumes the first request. */
static bool s_turn_terminal_cancel_pending = false;

typedef struct {
    int64_t pressed_at_us;
    int64_t authorized_at_us;
    int64_t capture_started_at_us;
    int64_t first_pcm_at_us;
    int64_t first_queued_at_us;
    int64_t first_opus_at_us;
    uint64_t frames_queued_before;
    uint64_t frames_sent_before;
    uint64_t queue_drops_before;
    uint64_t stale_drops_before;
    uint32_t queue_peak_depth;
    uint32_t max_opus_encode_us;
    uint32_t max_network_send_us;
} uplink_turn_timing_t;

/* The audio-manager callback and uplink task share these few diagnostics under
 * s_lock. They never retain PCM or touch the network path. */
static uplink_turn_timing_t s_turn_timing = {0};

static void uplink_set_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.last_error = error;
    portEXIT_CRITICAL(&s_lock);
}

static int64_t uplink_elapsed_ms(int64_t start_us, int64_t end_us)
{
    if ((start_us <= 0) || (end_us < start_us)) {
        return -1;
    }
    return (end_us - start_us) / 1000;
}

static void uplink_log_turn_summary(
    uint32_t session_generation,
    uint32_t ptt_generation,
    int64_t capture_stopped_at_us)
{
    uplink_turn_timing_t timing = {0};
    voice_assistant_uplink_status_t status = {0};

    portENTER_CRITICAL(&s_lock);
    timing = s_turn_timing;
    status = s_status;
    portEXIT_CRITICAL(&s_lock);

    APP_LOGI(
        TAG,
        TURN_SUMMARY_7E59B72F,
        "turn summary generation=%u ptt_generation=%u ptt_to_auth_ms=%" PRIi64
        " ptt_to_capture_ms=%" PRIi64
        " capture_to_first_pcm_ms=%" PRIi64
        " capture_to_first_queue_ms=%" PRIi64
        " capture_to_first_opus_ms=%" PRIi64
        " capture_to_stop_ms=%" PRIi64
        " frames_queued=%" PRIu64 " frames_sent=%" PRIu64
        " queue_drops=%" PRIu64 " stale_drops=%" PRIu64
        " queue_peak=%u/%u max_encode_us=%u max_send_us=%u",
        (unsigned)session_generation,
        (unsigned)ptt_generation,
        uplink_elapsed_ms(timing.pressed_at_us, timing.authorized_at_us),
        uplink_elapsed_ms(timing.pressed_at_us, timing.capture_started_at_us),
        uplink_elapsed_ms(timing.capture_started_at_us, timing.first_pcm_at_us),
        uplink_elapsed_ms(timing.capture_started_at_us, timing.first_queued_at_us),
        uplink_elapsed_ms(timing.capture_started_at_us, timing.first_opus_at_us),
        uplink_elapsed_ms(timing.capture_started_at_us, capture_stopped_at_us),
        status.frames_queued - timing.frames_queued_before,
        status.frames_sent - timing.frames_sent_before,
        status.frames_dropped_queue_full - timing.queue_drops_before,
        status.frames_dropped_stale - timing.stale_drops_before,
        (unsigned)timing.queue_peak_depth,
        (unsigned)UPLINK_QUEUE_LENGTH,
        (unsigned)timing.max_opus_encode_us,
        (unsigned)timing.max_network_send_us);
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
    const int64_t callback_time_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    accept = s_status.turn_active &&
             (s_status.session_generation == frame->stream_generation);
    if (accept && (s_turn_timing.first_pcm_at_us == 0)) {
        s_turn_timing.first_pcm_at_us = callback_time_us;
    }
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

    const UBaseType_t queue_depth = uxQueueMessagesWaiting(s_queue);
    portENTER_CRITICAL(&s_lock);
    ++s_status.frames_queued;
    if (s_turn_timing.first_queued_at_us == 0) {
        s_turn_timing.first_queued_at_us = callback_time_us;
    }
    if ((uint32_t)queue_depth > s_turn_timing.queue_peak_depth) {
        s_turn_timing.queue_peak_depth = (uint32_t)queue_depth;
    }
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

static bool uplink_tls_psram_ready(void)
{
    const size_t psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if ((psram_free < UPLINK_MIN_TLS_PSRAM_HEADROOM_BYTES) ||
        (psram_largest < UPLINK_MIN_TLS_PSRAM_HEADROOM_BYTES)) {
        APP_LOGW(TAG, TLS_PSRAM_HEADROOM_REJECTED_C0FF02B3,
                 "turn rejected before transport: psram_free=%u psram_largest=%u required=%u",
                 (unsigned)psram_free,
                 (unsigned)psram_largest,
                 (unsigned)UPLINK_MIN_TLS_PSRAM_HEADROOM_BYTES);
        return false;
    }

    APP_LOGI(TAG, TLS_PSRAM_HEADROOM_READY_C6FB5F7A,
             "TLS PSRAM ready: free=%u largest=%u required=%u",
             (unsigned)psram_free,
             (unsigned)psram_largest,
             (unsigned)UPLINK_MIN_TLS_PSRAM_HEADROOM_BYTES);
    return true;
}

static esp_err_t uplink_begin_turn(uint32_t generation)
{
    if (generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (voice_assistant_downlink_is_busy()) {
        return ESP_ERR_INVALID_STATE;
    }

    voice_assistant_ptt_status_t ptt = {0};
    if ((voice_assistant_ptt_get_status(&ptt) != ESP_OK) ||
        (ptt.state != VOICE_ASSISTANT_PTT_AUTHORIZED) ||
        !ptt.capture_authorized ||
        (ptt.session_generation != generation) ||
        (ptt.ptt_generation == 0U)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!uplink_tls_psram_ready()) {
        return ESP_ERR_NO_MEM;
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
        APP_LOGI(TAG, TURN_CANCELLED_BEFORE_CAPTUR_CD99854E,
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

    ret = voice_assistant_playback_mark_turn_started(ptt.ptt_generation);
    if (ret != ESP_OK) {
        (void)audio_manager_stream_disarm(generation);
        (void)xiaozhi_foundation_audio_uplink_stop(generation);
        (void)xiaozhi_foundation_audio_channel_close(generation);
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.turn_active = true;
    s_status.session_generation = generation;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    portENTER_CRITICAL(&s_lock);
    s_turn_timing = (uplink_turn_timing_t) {
        .pressed_at_us = ptt.pressed_at_us,
        .authorized_at_us = ptt.authorized_at_us,
        .frames_queued_before = s_status.frames_queued,
        .frames_sent_before = s_status.frames_sent,
        .queue_drops_before = s_status.frames_dropped_queue_full,
        .stale_drops_before = s_status.frames_dropped_stale,
    };
    portEXIT_CRITICAL(&s_lock);
    s_pcm_frame_samples = 0U;
    s_turn_packets = 0U;
    s_turn_opus_bytes = 0U;
    s_turn_pcm_samples = 0U;
    s_turn_ptt_generation = ptt.ptt_generation;
    s_turn_terminal_cancel_pending = false;

    ret = voice_assistant_audio_capture_start();
    if (ret != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.turn_active = false;
        portEXIT_CRITICAL(&s_lock);
        (void)audio_manager_stream_disarm(generation);
        (void)xiaozhi_foundation_audio_uplink_stop(generation);
        (void)xiaozhi_foundation_audio_channel_close(generation);
        (void)voice_assistant_playback_finish_turn(ptt.ptt_generation);
        s_turn_ptt_generation = 0U;
        return ret;
    }

    const int64_t capture_started_at_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_turn_timing.capture_started_at_us = capture_started_at_us;
    portEXIT_CRITICAL(&s_lock);

    ret = voice_recording_critical_enter(generation, ptt.ptt_generation);
    if (ret != ESP_OK) {
        APP_LOGE(TAG, RECORDING_CRITICAL_ENTRY_FAILED_15C761B9,
                 "recording critical entry failed generation=%u ptt_generation=%u error=%s",
                 (unsigned)generation,
                 (unsigned)ptt.ptt_generation,
                 esp_err_to_name(ret));
        portENTER_CRITICAL(&s_lock);
        s_status.turn_active = false;
        portEXIT_CRITICAL(&s_lock);
        (void)voice_assistant_audio_capture_stop();
        (void)audio_manager_stream_disarm(generation);
        (void)xiaozhi_foundation_audio_uplink_stop(generation);
        (void)xiaozhi_foundation_audio_channel_close(generation);
        (void)voice_assistant_playback_finish_turn(ptt.ptt_generation);
        s_turn_ptt_generation = 0U;
        return ret;
    }
    s_turn_critical_active = true;

    APP_LOGI(TAG, TURN_START_GENERATION_U_2D36C6A2,
             "turn START generation=%u recording_critical=1",
             (unsigned)generation);
    return ESP_OK;
}

static esp_err_t uplink_end_turn(uint32_t generation)
{
    const uint32_t ptt_generation = s_turn_ptt_generation;
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
        ret = voice_assistant_downlink_begin_response_wait(
            generation,
            ptt_generation);
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

    ret = voice_assistant_audio_capture_stop();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE) &&
        (first_error == ESP_OK)) {
        first_error = ret;
    }

    const int64_t capture_stopped_at_us = esp_timer_get_time();
    if (s_turn_critical_active) {
        const esp_err_t critical_exit_ret = voice_recording_critical_exit(
            generation, ptt_generation);
        if ((critical_exit_ret != ESP_OK) && (first_error == ESP_OK)) {
            first_error = critical_exit_ret;
        }
        s_turn_critical_active = false;
    }
    uplink_log_turn_summary(
        generation, ptt_generation, capture_stopped_at_us);

    /* Stop listening and retain the channel only while downlink owns the
     * bounded response wait. A zero-packet turn cannot produce a valid
     * response and must not block the next PTT attempt. */
    ret = response_wait_started
              ? xiaozhi_foundation_audio_uplink_stop_for_response(generation)
              : xiaozhi_foundation_audio_uplink_stop(generation);
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


    if (!response_wait_started || close_after_stop) {
        (void)voice_assistant_playback_finish_turn(ptt_generation);
    }
    s_turn_ptt_generation = 0U;
    s_turn_terminal_cancel_pending = false;

    (void)xQueueReset(s_queue);
    s_pcm_frame_samples = 0U;
    APP_LOGI(TAG, TURN_STOP_GENERATION_U_RESUL_56863FFB,
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
    const int64_t encode_started_us = esp_timer_get_time();
    esp_err_t ret = voice_assistant_opus_encode(
        samples,
        sample_count,
        s_opus_packet,
        sizeof(s_opus_packet),
        &packet_size);
    const int64_t encode_duration_us = esp_timer_get_time() - encode_started_us;
    if (encode_duration_us > 0) {
        portENTER_CRITICAL(&s_lock);
        if ((uint64_t)encode_duration_us > s_turn_timing.max_opus_encode_us) {
            s_turn_timing.max_opus_encode_us =
                (encode_duration_us > UINT32_MAX)
                    ? UINT32_MAX
                    : (uint32_t)encode_duration_us;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    const int64_t send_started_us = esp_timer_get_time();
    ret = xiaozhi_foundation_audio_uplink_send_opus_packet(
        generation, s_opus_packet, packet_size);
    const int64_t send_duration_us = esp_timer_get_time() - send_started_us;
    if (send_duration_us > 0) {
        portENTER_CRITICAL(&s_lock);
        if ((uint64_t)send_duration_us > s_turn_timing.max_network_send_us) {
            s_turn_timing.max_network_send_us =
                (send_duration_us > UINT32_MAX)
                    ? UINT32_MAX
                    : (uint32_t)send_duration_us;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    if (ret == ESP_OK) {
        ++s_turn_packets;
        s_turn_opus_bytes += packet_size;
        s_turn_pcm_samples += sample_count;
        portENTER_CRITICAL(&s_lock);
        ++s_status.frames_sent;
        portEXIT_CRITICAL(&s_lock);
        if (s_turn_packets == 1U) {
            portENTER_CRITICAL(&s_lock);
            s_turn_timing.first_opus_at_us = esp_timer_get_time();
            portEXIT_CRITICAL(&s_lock);
            APP_LOGI(TAG, FIRST_OPUS_PACKET_GENERATION_BA47105F,
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
            APP_LOGE(TAG, TURN_START_FAILED_S_98355C4A, "turn start failed: %s", esp_err_to_name(ret));
            uplink_set_error(ret);
            /* Do not retry a resource-rejected turn every 20 ms while the
             * physical button remains held. The next deliberate press may
             * retry after memory pressure has cleared. */
            (void)voice_assistant_ptt_cancel();
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
        return;
    }

    if (active) {
        audio_manager_status_t audio = {0};
        xiaozhi_foundation_session_status_t session = {0};
        const esp_err_t audio_ret = audio_manager_get_status(&audio);
        const esp_err_t session_ret =
            xiaozhi_foundation_session_get_status(&session);
        const bool capture_lost =
            (audio_ret != ESP_OK) || !audio.capture_i2s_active;
        const bool transport_lost =
            (session_ret != ESP_OK) || !session.active ||
            (session.client_generation != generation);

        /* A terminal audio or transport transition can arrive without a PCM
         * callback. Revoke the PTT intent so the normal end-turn path closes
         * the runtime critical window instead of leaving it active until the
         * physical release. This does not stop Wi-Fi/TCPIP or interrupt an
         * in-flight background operation. */
        if ((capture_lost || transport_lost) &&
            !s_turn_terminal_cancel_pending) {
            s_turn_terminal_cancel_pending = true;
            const esp_err_t error =
                capture_lost
                    ? ((audio_ret == ESP_OK) ? ESP_ERR_INVALID_STATE : audio_ret)
                    : ((session_ret == ESP_OK) ? ESP_ERR_INVALID_STATE : session_ret);
            APP_LOGW(TAG, TURN_CANCELLED_AFTER_CAPTURE_OR_85E29BBA,
                     "turn cancelled after capture/transport loss generation=%u capture_lost=%s transport_lost=%s error=%s",
                     (unsigned)generation,
                     capture_lost ? "yes" : "no",
                     transport_lost ? "yes" : "no",
                     esp_err_to_name(error));
            uplink_set_error(error);
            (void)voice_assistant_ptt_cancel();
        }
    }
}

static void uplink_task(void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    APP_LOGI(TAG, COORDINATOR_STARTED_E5C98CE5, "coordinator started");

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
                    APP_LOGE(TAG, OPUS_TX_FAILED_GENERATION_U_E9E4ECC6,
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

    const size_t queue_storage_bytes =
        (size_t)UPLINK_QUEUE_LENGTH * sizeof(uplink_frame_item_t);
    s_queue_storage = (uint8_t *)heap_caps_malloc(
        queue_storage_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue_storage == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_queue = xQueueCreateStatic(
        UPLINK_QUEUE_LENGTH,
        sizeof(uplink_frame_item_t),
        s_queue_storage,
        &s_queue_control);
    if (s_queue == NULL) {
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.last_error = ESP_OK;

    const esp_err_t codec_ret = voice_assistant_opus_encoder_init();
    if (codec_ret != ESP_OK) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
        return codec_ret;
    }

    const esp_err_t ret = audio_manager_stream_register_callback(
        uplink_stream_callback, NULL);
    if (ret != ESP_OK) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        heap_caps_free(s_queue_storage);
        s_queue_storage = NULL;
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
