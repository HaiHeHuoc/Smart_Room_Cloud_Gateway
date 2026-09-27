#include "wake_word_manager.h"

#include "sdkconfig.h"

#if CONFIG_WAKE_WORD_ENABLE

#include <stdatomic.h>
#include <string.h>

#include "audio_manager.h"
#include "audio_manager_arbitration.h"
#include "audio_manager_capture_arbiter.h"
#include "audio_manager_stream.h"
#include "app_log.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"
#include "voice_recording_critical.h"
#include "wake_word_runtime_policy.h"

#define WAKE_WORD_FEED_TASK_NAME "wake_word_feed"
#define WAKE_WORD_FETCH_TASK_NAME "wake_word_fetch"
#define WAKE_WORD_TASK_STACK 6144U
#define WAKE_WORD_TASK_PRIORITY 5U
#define WAKE_WORD_AFE_ENGINE_PRIORITY 6U
#define WAKE_WORD_AFE_ENGINE_CORE 1
#define WAKE_WORD_AFE_ENGINE_STACK_BYTES (8U * 1024U)
#define WAKE_WORD_AFE_INTERNAL_HEADROOM_BYTES (2U * 1024U)
#define WAKE_WORD_RX_WAIT_MS 100U
#define WAKE_WORD_NO_SPEECH_TIMEOUT_MS 3000U
#define WAKE_WORD_END_SILENCE_MS 800U
#define WAKE_WORD_MAX_UTTERANCE_MS 20000U
#define WAKE_WORD_MAX_TURN_MS 30000U
#define WAKE_WORD_REARM_GUARD_MS 400U
#define WAKE_WORD_REQUEST_ID 0x24170001U
#define WAKE_WORD_MODEL_NAME "wn9_hiesp"
#define WAKE_WORD_MODEL_PARTITION "model"
#define WAKE_WORD_MAX_FEED_SAMPLES 1024U

static const char *const TAG = "WAKE_WORD";

typedef struct {
    uint64_t last_afe_feed_frame_sequence;
    uint64_t pcm_frames_received;
    uint64_t afe_feed_calls;
    uint64_t afe_fetch_calls;
    uint64_t afe_feed_failures;
    uint64_t afe_fetch_failures;
    uint64_t afe_feed_dropped_reset;
    uint32_t max_afe_feed_us;
    uint32_t max_afe_fetch_us;
} wake_word_afe_metrics_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t feed_task;
    TaskHandle_t fetch_task;
    srmodel_list_t *models;
    const esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe;
    int16_t feed_buffer[WAKE_WORD_MAX_FEED_SAMPLES];
    size_t feed_fill;
    size_t feed_samples;
    atomic_uint_fast32_t afe_reset_generation;
    atomic_uint_fast32_t afe_reset_applied_generation;
    atomic_bool afe_feed_in_progress;
    atomic_bool afe_processing_active;
    atomic_bool afe_input_ready;
    atomic_bool handsfree_handoff_active;
    uint64_t last_frame_sequence;
    int64_t next_capture_check_us;
    int64_t rearm_after_us;
    int64_t wake_started_us;
    int64_t speech_started_us;
    int64_t silence_started_us;
    wake_word_manager_event_callback_t callback;
    void *callback_context;
    wake_word_manager_status_t status;
} wake_word_runtime_t;

static wake_word_runtime_t s_runtime;
static portMUX_TYPE s_afe_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static wake_word_afe_metrics_t s_afe_metrics = {0};

_Static_assert(WAKE_WORD_END_SILENCE_MS < WAKE_WORD_MAX_UTTERANCE_MS,
               "end silence must precede max utterance");
_Static_assert(WAKE_WORD_MAX_UTTERANCE_MS < WAKE_WORD_MAX_TURN_MS,
               "max utterance must precede max turn");
_Static_assert(WAKE_WORD_NO_SPEECH_TIMEOUT_MS < WAKE_WORD_MAX_TURN_MS,
               "no speech must precede max turn");

static bool wake_word_take_lock(void)
{
    return (s_runtime.lock != NULL) &&
           (xSemaphoreTake(s_runtime.lock, pdMS_TO_TICKS(100U)) == pdTRUE);
}

static bool wake_word_speaker_is_active(void)
{
    audio_manager_playback_status_t playback = {0};
    if (audio_manager_get_playback_status(&playback) != ESP_OK) {
        return false;
    }
    return (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING) ||
           (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING) ||
           (playback.state == AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING);
}

static void wake_word_note_feed_metrics(uint64_t frame_sequence,
                                        int feed_result,
                                        int64_t duration_us)
{
    portENTER_CRITICAL(&s_afe_metrics_lock);
    ++s_afe_metrics.afe_feed_calls;
    if (feed_result >= 0) {
        s_afe_metrics.last_afe_feed_frame_sequence = frame_sequence;
    } else {
        ++s_afe_metrics.afe_feed_failures;
    }
    if ((duration_us > 0) && ((uint64_t)duration_us > s_afe_metrics.max_afe_feed_us)) {
        s_afe_metrics.max_afe_feed_us = (duration_us > UINT32_MAX)
            ? UINT32_MAX : (uint32_t)duration_us;
    }
    portEXIT_CRITICAL(&s_afe_metrics_lock);
}

static void wake_word_note_fetch_metrics(bool failed, int64_t duration_us)
{
    portENTER_CRITICAL(&s_afe_metrics_lock);
    ++s_afe_metrics.afe_fetch_calls;
    if (failed) {
        ++s_afe_metrics.afe_fetch_failures;
    }
    if ((duration_us > 0) && ((uint64_t)duration_us > s_afe_metrics.max_afe_fetch_us)) {
        s_afe_metrics.max_afe_fetch_us = (duration_us > UINT32_MAX)
            ? UINT32_MAX : (uint32_t)duration_us;
    }
    portEXIT_CRITICAL(&s_afe_metrics_lock);
}

static void wake_word_note_pcm_frame_received(void)
{
    portENTER_CRITICAL(&s_afe_metrics_lock);
    ++s_afe_metrics.pcm_frames_received;
    portEXIT_CRITICAL(&s_afe_metrics_lock);
}

static void wake_word_note_reset_drop(void)
{
    portENTER_CRITICAL(&s_afe_metrics_lock);
    ++s_afe_metrics.afe_feed_dropped_reset;
    portEXIT_CRITICAL(&s_afe_metrics_lock);
}

/* ESP-SR documents feed and fetch as independent producer/consumer tasks.
 * Resets therefore run only on the fetch side after it has observed a
 * generation request; feed drops incomplete local data until that reset has
 * completed. This avoids resetting the AFE ring buffer beside an active fetch. */
static void wake_word_request_afe_reset(void)
{
    atomic_store_explicit(&s_runtime.afe_processing_active, false, memory_order_release);
    atomic_store_explicit(&s_runtime.afe_input_ready, false, memory_order_release);
    (void)atomic_fetch_add_explicit(&s_runtime.afe_reset_generation, 1U,
                                    memory_order_acq_rel);
    if (s_runtime.fetch_task != NULL) {
        xTaskNotifyGive(s_runtime.fetch_task);
    }
}

static void wake_word_set_afe_processing_active(bool active)
{
    atomic_store_explicit(&s_runtime.afe_processing_active, active, memory_order_release);
    if (s_runtime.fetch_task != NULL) {
        xTaskNotifyGive(s_runtime.fetch_task);
    }
}

static void wake_word_emit_event(wake_word_manager_event_kind_t kind,
                                 uint64_t frame_sequence,
                                 uint32_t duration_ms)
{
    wake_word_manager_event_callback_t callback = NULL;
    void *context = NULL;
    wake_word_manager_event_t event = {0};
    if (!wake_word_take_lock()) return;
    event.kind = (uint32_t)kind;
    event.state = (uint32_t)s_runtime.status.state;
    event.generation = s_runtime.status.wake_generation;
    event.duration_ms = duration_ms;
    event.frame_sequence = frame_sequence;
    event.timestamp_us = (uint64_t)esp_timer_get_time();
    event.detection_count = s_runtime.status.detections;
    callback = s_runtime.callback;
    context = s_runtime.callback_context;
    xSemaphoreGive(s_runtime.lock);
    if (callback != NULL) callback(&event, context);
}

static void wake_word_enter_rearm(wake_word_manager_event_kind_t event,
                                  uint64_t frame_sequence, int64_t now_us,
                                  bool emit_event)
{
    uint32_t duration_ms = 0U;
    wake_word_manager_state_t previous = WAKE_WORD_STATE_DISABLED;
    uint32_t generation = 0U;
    if (!wake_word_take_lock()) return;
    if (s_runtime.wake_started_us != 0) duration_ms =
        (uint32_t)((now_us - s_runtime.wake_started_us) / 1000LL);
    previous = s_runtime.status.state;
    generation = s_runtime.status.wake_generation;
    s_runtime.status.state = wake_word_runtime_next_state(
        previous, WAKE_WORD_RUNTIME_TRIGGER_REARM);
    s_runtime.status.last_transition_us = (uint64_t)now_us;
    s_runtime.status.last_utterance_duration_ms = duration_ms;
    s_runtime.rearm_after_us = now_us + ((int64_t)WAKE_WORD_REARM_GUARD_MS * 1000LL);
    s_runtime.speech_started_us = 0;
    s_runtime.silence_started_us = 0;
    xSemaphoreGive(s_runtime.lock);
    wake_word_request_afe_reset();
    APP_LOGI(TAG, WAKE_REARM_GUARD_G_U_8F943F6A,
             "WAKE state %u -> REARM_GUARD gen=%lu reason=%u",
             (unsigned)previous, (unsigned long)generation, (unsigned)event);
    if (emit_event) {
        wake_word_emit_event(event, frame_sequence, duration_ms);
    }
}

static void wake_word_complete_utterance(wake_word_manager_event_kind_t event,
                                         uint64_t frame_sequence, int64_t now_us)
{
    wake_word_manager_state_t previous = WAKE_WORD_STATE_DISABLED;
    uint32_t generation = 0U;
    uint32_t duration_ms = 0U;
    if (!wake_word_take_lock()) {
        return;
    }
    previous = s_runtime.status.state;
    generation = s_runtime.status.wake_generation;
    if (s_runtime.wake_started_us != 0) {
        duration_ms = (uint32_t)((now_us - s_runtime.wake_started_us) / 1000LL);
    }
    s_runtime.status.state = wake_word_runtime_next_state(
        previous, WAKE_WORD_RUNTIME_TRIGGER_UTTERANCE_COMPLETE);
    s_runtime.status.last_transition_us = (uint64_t)now_us;
    s_runtime.status.last_utterance_duration_ms = duration_ms;
    if (event == WAKE_WORD_EVENT_UTTERANCE_COMPLETE) {
        ++s_runtime.status.utterance_complete_count;
    } else {
        ++s_runtime.status.max_utterance_timeout_count;
    }
    xSemaphoreGive(s_runtime.lock);

    APP_LOGI(TAG, WAKE_UTTERANCE_COMPLETE_G_54B56FD7,
             "WAKE state %u -> UTTERANCE_COMPLETE gen=%lu reason=%u duration_ms=%lu",
             (unsigned)previous, (unsigned long)generation, (unsigned)event,
             (unsigned long)duration_ms);
    wake_word_emit_event(event, frame_sequence, duration_ms);

    /* A callback may mute/disable this manager. Only move this exact local
     * generation into its guard; never overwrite a newer policy decision. */
    if (!wake_word_take_lock()) {
        return;
    }
    if ((s_runtime.status.state != WAKE_WORD_STATE_UTTERANCE_COMPLETE) ||
        (s_runtime.status.wake_generation != generation)) {
        xSemaphoreGive(s_runtime.lock);
        return;
    }
    s_runtime.status.state = wake_word_runtime_next_state(
        WAKE_WORD_STATE_UTTERANCE_COMPLETE, WAKE_WORD_RUNTIME_TRIGGER_REARM);
    s_runtime.status.last_transition_us = (uint64_t)now_us;
    s_runtime.rearm_after_us = now_us + ((int64_t)WAKE_WORD_REARM_GUARD_MS * 1000LL);
    s_runtime.speech_started_us = 0;
    s_runtime.silence_started_us = 0;
    xSemaphoreGive(s_runtime.lock);
    wake_word_request_afe_reset();
    APP_LOGI(TAG, WAKE_UTTERANCE_REARM_G_01BFA655,
             "WAKE state UTTERANCE_COMPLETE -> REARM_GUARD gen=%lu",
             (unsigned long)generation);
}

static void wake_word_note_suppression(bool playback);
static void wake_word_ensure_capture_request(int64_t now_us);

static void wake_word_tick(int64_t now_us)
{
    /* Capture can be preempted before another PCM frame reaches this worker.
     * Poll the existing ownership signals at the bounded receive interval so a
     * PTT/playback transition is reflected even while the queue is empty. */
    if (voice_recording_critical_is_active()) {
        if (!atomic_load_explicit(&s_runtime.handsfree_handoff_active,
                                  memory_order_acquire)) {
            wake_word_note_suppression(false);
        }
        return;
    }
    if (wake_word_speaker_is_active()) {
        wake_word_note_suppression(true);
        return;
    }
    wake_word_ensure_capture_request(now_us);
    if (!wake_word_take_lock()) return;
    const wake_word_manager_state_t state = s_runtime.status.state;
    const int64_t started = s_runtime.wake_started_us;
    if (state == WAKE_WORD_STATE_SUPPRESSED) {
        xSemaphoreGive(s_runtime.lock);
        wake_word_enter_rearm(WAKE_WORD_EVENT_CANCELLED, 0U, now_us, false);
        return;
    }
    if (state == WAKE_WORD_STATE_REARM_GUARD && now_us >= s_runtime.rearm_after_us) {
        const bool should_rearm = s_runtime.status.enabled && !s_runtime.status.muted;
        s_runtime.status.state = should_rearm
            ? wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_REARMED)
            : WAKE_WORD_STATE_DISABLED;
        s_runtime.status.last_transition_us = (uint64_t)now_us;
        ++s_runtime.status.rearm_count;
        const uint32_t generation = s_runtime.status.wake_generation;
        xSemaphoreGive(s_runtime.lock);
        if (should_rearm) {
            wake_word_set_afe_processing_active(true);
        }
        APP_LOGI(TAG, WAKE_REARMED_G_5F7B2458,
                 "WAKE state REARM_GUARD -> ARMED gen=%lu",
                 (unsigned long)generation);
        wake_word_emit_event(WAKE_WORD_EVENT_REARMED, 0U, 0U);
        return;
    }
    if ((state == WAKE_WORD_STATE_WAIT_FOR_SPEECH) &&
        ((now_us - started) >= ((int64_t)WAKE_WORD_NO_SPEECH_TIMEOUT_MS * 1000LL))) {
        ++s_runtime.status.no_speech_timeout_count;
        xSemaphoreGive(s_runtime.lock);
        wake_word_enter_rearm(WAKE_WORD_EVENT_NO_SPEECH_TIMEOUT, 0U, now_us, true);
        return;
    }
    if ((state == WAKE_WORD_STATE_LISTENING) && (s_runtime.speech_started_us != 0) &&
        ((now_us - s_runtime.speech_started_us) >=
         ((int64_t)WAKE_WORD_MAX_UTTERANCE_MS * 1000LL))) {
        xSemaphoreGive(s_runtime.lock);
        wake_word_complete_utterance(WAKE_WORD_EVENT_MAX_UTTERANCE, 0U, now_us);
        return;
    }
    if ((state == WAKE_WORD_STATE_LISTENING) &&
        ((now_us - started) >= ((int64_t)WAKE_WORD_MAX_TURN_MS * 1000LL))) {
        ++s_runtime.status.cancel_count;
        ++s_runtime.status.max_total_turn_timeout_count;
        xSemaphoreGive(s_runtime.lock);
        wake_word_enter_rearm(WAKE_WORD_EVENT_CANCELLED, 0U, now_us, true);
        return;
    }
    xSemaphoreGive(s_runtime.lock);
}

static void wake_word_note_suppression(bool playback)
{
    const bool handoff_was_active = atomic_exchange_explicit(
        &s_runtime.handsfree_handoff_active, false, memory_order_acq_rel);
    if (handoff_was_active) {
        (void)audio_manager_stream_local_monitor_set_allow_during_ptt(false);
    }
    if (!wake_word_take_lock()) {
        return;
    }
    s_runtime.status.handsfree_handoff_active = false;
    if (!s_runtime.status.enabled || s_runtime.status.muted ||
        (s_runtime.status.state == WAKE_WORD_STATE_SUPPRESSED)) {
        xSemaphoreGive(s_runtime.lock);
        return;
    }
    const wake_word_manager_state_t previous = s_runtime.status.state;
    const uint32_t generation = s_runtime.status.wake_generation;
    const bool cancel = (previous == WAKE_WORD_STATE_WAIT_FOR_SPEECH) ||
                        (previous == WAKE_WORD_STATE_LISTENING);
    if (playback) {
        ++s_runtime.status.pcm_suppressed_playback;
        ++s_runtime.status.playback_suppression_count;
    } else {
        ++s_runtime.status.pcm_suppressed_recording;
        ++s_runtime.status.recording_suppression_count;
    }
    if (cancel) {
        ++s_runtime.status.cancel_count;
    }
    s_runtime.status.state = wake_word_runtime_next_state(
        previous, WAKE_WORD_RUNTIME_TRIGGER_SUPPRESS);
    s_runtime.status.last_transition_us = (uint64_t)esp_timer_get_time();
    xSemaphoreGive(s_runtime.lock);
    wake_word_request_afe_reset();
    APP_LOGI(TAG, WAKE_SUPPRESSED_G_U_7C1F51C1,
             "WAKE state %u -> SUPPRESSED gen=%lu reason=%s",
             (unsigned)previous, (unsigned long)generation,
             playback ? "playback" : "recording");
    if (cancel) {
        wake_word_emit_event(WAKE_WORD_EVENT_CANCELLED, 0U, 0U);
    }
}

static void wake_word_emit_detection(uint64_t frame_sequence,
                                     const afe_fetch_result_t *result)
{
    wake_word_manager_event_callback_t callback = NULL;
    void *callback_context = NULL;
    wake_word_manager_event_t event = {0};
    if (!wake_word_take_lock()) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if ((s_runtime.status.state != WAKE_WORD_STATE_ARMED) ||
        (now_us < s_runtime.rearm_after_us)) {
        ++s_runtime.status.debounce_suppressed;
        xSemaphoreGive(s_runtime.lock);
        return;
    }
    ++s_runtime.status.detections;
    ++s_runtime.status.wake_generation;
    if (s_runtime.status.wake_generation == 0U) ++s_runtime.status.wake_generation;
    s_runtime.status.last_wake_word_index = (uint32_t)result->wake_word_index;
    s_runtime.status.state = wake_word_runtime_next_state(
        s_runtime.status.state, WAKE_WORD_RUNTIME_TRIGGER_WAKE);
    s_runtime.status.last_transition_us = (uint64_t)now_us;
    s_runtime.status.last_wake_us = (uint64_t)now_us;
    s_runtime.wake_started_us = now_us;
    event.kind = WAKE_WORD_EVENT_WAKE_DETECTED;
    event.state = WAKE_WORD_STATE_WAIT_FOR_SPEECH;
    event.generation = s_runtime.status.wake_generation;
    event.frame_sequence = frame_sequence;
    event.timestamp_us = (uint64_t)now_us;
    event.wake_word_index = (uint32_t)result->wake_word_index;
    event.detection_count = s_runtime.status.detections;
    callback = s_runtime.callback;
    callback_context = s_runtime.callback_context;
    xSemaphoreGive(s_runtime.lock);

    APP_LOGI(TAG, WAKE_DETECTED_G_U_FRAME_L0A4A56B,
             "WAKE state ARMED -> WAIT_FOR_SPEECH gen=%lu word=%u frame=%llu",
             (unsigned long)event.generation, (unsigned)event.wake_word_index,
             (unsigned long long)event.frame_sequence);
    /* The callback runs after releasing manager state. Application composition
     * must only copy/queue this event; it must not run PTT/Xiaozhi work in the
     * AFE fetch worker. */
    if (callback != NULL) {
        callback(&event, callback_context);
    }
}

static void wake_word_process_vad(uint64_t frame_sequence,
                                  const afe_fetch_result_t *result)
{
    const int64_t now_us = esp_timer_get_time();
    if (!wake_word_take_lock()) return;
    const wake_word_manager_state_t state = s_runtime.status.state;
    if ((state == WAKE_WORD_STATE_WAIT_FOR_SPEECH) &&
        (result->vad_state == VAD_SPEECH)) {
        s_runtime.status.state = wake_word_runtime_next_state(
            state, WAKE_WORD_RUNTIME_TRIGGER_SPEECH);
        ++s_runtime.status.speech_start_count;
        s_runtime.speech_started_us = now_us;
        s_runtime.silence_started_us = 0;
        s_runtime.status.last_transition_us = (uint64_t)now_us;
        s_runtime.status.last_speech_start_us = (uint64_t)now_us;
        const uint32_t generation = s_runtime.status.wake_generation;
        xSemaphoreGive(s_runtime.lock);
        APP_LOGI(TAG, WAKE_SPEECH_STARTED_G_0B3B6B1A,
                 "WAKE state WAIT_FOR_SPEECH -> LISTENING gen=%lu reason=VAD_SPEECH",
                 (unsigned long)generation);
        wake_word_emit_event(WAKE_WORD_EVENT_SPEECH_STARTED, frame_sequence, 0U);
        return;
    }
    if (state != WAKE_WORD_STATE_LISTENING) {
        xSemaphoreGive(s_runtime.lock);
        return;
    }
    if (result->vad_state == VAD_SPEECH) {
        s_runtime.silence_started_us = 0;
    } else if (s_runtime.silence_started_us == 0) {
        s_runtime.silence_started_us = now_us;
    } else if ((now_us - s_runtime.silence_started_us) >=
               ((int64_t)WAKE_WORD_END_SILENCE_MS * 1000LL)) {
        xSemaphoreGive(s_runtime.lock);
        wake_word_complete_utterance(WAKE_WORD_EVENT_UTTERANCE_COMPLETE,
                                     frame_sequence, now_us);
        return;
    }
    xSemaphoreGive(s_runtime.lock);
}

static void wake_word_feed_frame(const audio_manager_stream_local_monitor_frame_t *frame)
{
    if ((frame->sample_rate_hz != AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ) ||
        (frame->channels != AUDIO_MANAGER_STREAM_CHANNELS) ||
        (frame->sample_count == 0U) ||
        (frame->sample_count > AUDIO_MANAGER_STREAM_FRAME_SAMPLES)) {
        return;
    }
    if (atomic_load_explicit(&s_runtime.afe_reset_generation, memory_order_acquire) !=
        atomic_load_explicit(&s_runtime.afe_reset_applied_generation, memory_order_acquire)) {
        s_runtime.feed_fill = 0U;
        wake_word_note_reset_drop();
        return;
    }
    for (size_t index = 0U; index < frame->sample_count; ++index) {
        s_runtime.feed_buffer[s_runtime.feed_fill++] = frame->samples[index];
        if (s_runtime.feed_fill != s_runtime.feed_samples) {
            continue;
        }
        atomic_store_explicit(&s_runtime.afe_feed_in_progress, true, memory_order_release);
        const uint32_t reset_generation = atomic_load_explicit(
            &s_runtime.afe_reset_generation, memory_order_acquire);
        if (reset_generation == atomic_load_explicit(&s_runtime.afe_reset_applied_generation,
                                                     memory_order_acquire)) {
            const int64_t started_us = esp_timer_get_time();
            const int fed = s_runtime.afe_iface->feed(s_runtime.afe, s_runtime.feed_buffer);
            const int64_t duration_us = esp_timer_get_time() - started_us;
            wake_word_note_feed_metrics(frame->frame_sequence, fed, duration_us);
            /* A zero-byte feed means the bounded AFE input ring could not
             * accept this chunk.  It still proves that the consumer must be
             * woken: waiting only for a later successful feed creates a
             * permanent full-ring deadlock after a timed-out fetch. */
            if (fed >= 0) {
                atomic_store_explicit(&s_runtime.afe_input_ready, true,
                                      memory_order_release);
                if (s_runtime.fetch_task != NULL) {
                    xTaskNotifyGive(s_runtime.fetch_task);
                }
            }
        } else {
            wake_word_note_reset_drop();
        }
        atomic_store_explicit(&s_runtime.afe_feed_in_progress, false, memory_order_release);
        s_runtime.feed_fill = 0U;
        if (reset_generation != atomic_load_explicit(&s_runtime.afe_reset_generation,
                                                     memory_order_acquire)) {
            return;
        }
    }
}

static uint64_t wake_word_last_afe_feed_frame_sequence(void)
{
    uint64_t sequence = 0U;
    portENTER_CRITICAL(&s_afe_metrics_lock);
    sequence = s_afe_metrics.last_afe_feed_frame_sequence;
    portEXIT_CRITICAL(&s_afe_metrics_lock);
    return sequence;
}

static void wake_word_apply_pending_afe_reset(uint32_t *reset_seen)
{
    if (reset_seen == NULL) {
        return;
    }
    const uint32_t requested = atomic_load_explicit(&s_runtime.afe_reset_generation,
                                                    memory_order_acquire);
    if (*reset_seen == requested) {
        return;
    }
    while (atomic_load_explicit(&s_runtime.afe_feed_in_progress, memory_order_acquire)) {
        taskYIELD();
    }
    if ((s_runtime.afe_iface != NULL) && (s_runtime.afe != NULL)) {
        (void)s_runtime.afe_iface->reset_buffer(s_runtime.afe);
    }
    s_runtime.feed_fill = 0U;
    atomic_store_explicit(&s_runtime.afe_input_ready, false, memory_order_release);
    *reset_seen = requested;
    atomic_store_explicit(&s_runtime.afe_reset_applied_generation, requested,
                          memory_order_release);
}

static void wake_word_fetch_task(void *context)
{
    (void)context;
    uint32_t reset_seen = atomic_load_explicit(&s_runtime.afe_reset_applied_generation,
                                               memory_order_acquire);
    for (;;) {
        wake_word_apply_pending_afe_reset(&reset_seen);
        if (!atomic_load_explicit(&s_runtime.afe_processing_active,
                                  memory_order_acquire)) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* AFE fetch has a two-second default timeout. Do not enter it before
         * a complete input chunk was accepted: that would manufacture an
         * underflow diagnostic during normal capture startup/rearm. */
        if (!atomic_load_explicit(&s_runtime.afe_input_ready,
                                  memory_order_acquire)) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        /* ESP-SR owns this blocking producer/consumer boundary: feed runs in
         * the PCM worker while this task continuously fetches AFE output.
         * Do not wait for a per-frame notification here; doing so lets a fast
         * microphone producer fill AFE's two-frame ring before fetch runs. */
        const int64_t started_us = esp_timer_get_time();
        afe_fetch_result_t *result = s_runtime.afe_iface->fetch(s_runtime.afe);
        const int64_t duration_us = esp_timer_get_time() - started_us;
        const bool failed = (result == NULL) || (result->ret_value == ESP_FAIL);
        const bool reset_or_inactive =
            (reset_seen != atomic_load_explicit(&s_runtime.afe_reset_generation,
                                                memory_order_acquire)) ||
            !atomic_load_explicit(&s_runtime.afe_processing_active,
                                  memory_order_acquire);
        wake_word_note_fetch_metrics(failed && !reset_or_inactive, duration_us);
        if (reset_or_inactive || failed) {
            if (failed && !reset_or_inactive) {
                /* Do not wait for a fresh *successful* feed here. The input
                 * ring can already be full, in which case feed() cannot
                 * succeed and the old recovery gate deadlocked the AFE. A
                 * generation reset is applied by this fetch worker before it
                 * accepts the next complete PCM chunk. */
                APP_LOGW(TAG, AFE_FETCH_RECOVERY_3ACB7E2D,
                         "AFE fetch returned %s; resetting bounded pipeline",
                         (result == NULL) ? "NULL" : "an error result");
                wake_word_request_afe_reset();
                wake_word_set_afe_processing_active(true);
            }
            continue;
        }
        const uint64_t sequence = wake_word_last_afe_feed_frame_sequence();
        if (result->wakeup_state == WAKENET_DETECTED) {
            wake_word_emit_detection(sequence, result);
        }
        wake_word_process_vad(sequence, result);
    }
}

static void wake_word_feed_task(void *context)
{
    (void)context;
    for (;;) {
        audio_manager_stream_local_monitor_frame_t frame = {0};
        const esp_err_t receive_ret = audio_manager_stream_local_monitor_receive(
            &frame, WAKE_WORD_RX_WAIT_MS);
        if (receive_ret == ESP_ERR_TIMEOUT) {
            wake_word_tick(esp_timer_get_time());
            continue;
        }
        if (receive_ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(WAKE_WORD_RX_WAIT_MS));
            continue;
        }
        wake_word_note_pcm_frame_received();
        if (!wake_word_take_lock()) {
            continue;
        }
        const bool enabled = s_runtime.status.enabled && !s_runtime.status.muted;
        if (!enabled) {
            ++s_runtime.status.pcm_dropped_disabled;
            xSemaphoreGive(s_runtime.lock);
            continue;
        }
        if ((s_runtime.last_frame_sequence != 0U) &&
            (frame.frame_sequence != s_runtime.last_frame_sequence + 1U)) {
            ++s_runtime.status.pcm_sequence_gaps;
            s_runtime.rearm_after_us = esp_timer_get_time() +
                ((int64_t)WAKE_WORD_REARM_GUARD_MS * 1000LL);
        }
        s_runtime.last_frame_sequence = frame.frame_sequence;
        xSemaphoreGive(s_runtime.lock);
        if (voice_recording_critical_is_active() &&
            !atomic_load_explicit(&s_runtime.handsfree_handoff_active,
                                  memory_order_acquire)) {
            wake_word_note_suppression(false);
            continue;
        }
        if (wake_word_speaker_is_active()) {
            wake_word_note_suppression(true);
            continue;
        }
        if (wake_word_take_lock()) {
            const bool suppressed = s_runtime.status.state == WAKE_WORD_STATE_SUPPRESSED;
            xSemaphoreGive(s_runtime.lock);
            if (suppressed) {
                wake_word_enter_rearm(WAKE_WORD_EVENT_CANCELLED,
                                      frame.frame_sequence,
                                      esp_timer_get_time(), false);
                continue;
            }
        }
        if (atomic_load_explicit(&s_runtime.afe_processing_active,
                                 memory_order_acquire)) {
            wake_word_feed_frame(&frame);
        }
        wake_word_tick(esp_timer_get_time());
    }
}

esp_err_t wake_word_manager_init(void)
{
    if (s_runtime.status.initialized) {
        return ESP_OK;
    }

    /* This admission check runs before the manager allocates any of its own
     * state. The application starts mandatory PTT/WebSocket first, so failure
     * here leaves the PTT-only product path untouched. */
    const size_t largest_internal_block = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_runtime.status.internal_largest_before_afe_bytes =
        (largest_internal_block > UINT32_MAX) ? UINT32_MAX :
        (uint32_t)largest_internal_block;
    if (largest_internal_block <
        (WAKE_WORD_AFE_ENGINE_STACK_BYTES +
         WAKE_WORD_AFE_INTERNAL_HEADROOM_BYTES)) {
        APP_LOGE(TAG, WAKE_WORD_AFE_INTERNAL_HEAP_TOO_SMALL_32B60F1A,
                 "Wake AFE needs a contiguous Internal-RAM block of at least %u B; largest=%u B",
                 (unsigned)(WAKE_WORD_AFE_ENGINE_STACK_BYTES +
                            WAKE_WORD_AFE_INTERNAL_HEADROOM_BYTES),
                 (unsigned)largest_internal_block);
        s_runtime.status.last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }

    s_runtime.lock = xSemaphoreCreateMutex();
    if (s_runtime.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_runtime.models = esp_srmodel_init(WAKE_WORD_MODEL_PARTITION);
    if ((s_runtime.models == NULL) ||
        (esp_srmodel_exists(s_runtime.models, WAKE_WORD_MODEL_NAME) < 0)) {
        s_runtime.status.last_error = ESP_ERR_NOT_FOUND;
        return ESP_ERR_NOT_FOUND;
    }

    afe_config_t *config = afe_config_init("M", s_runtime.models,
                                           AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (config == NULL) {
        s_runtime.status.last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    config->aec_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->vad_init = true;
    config->agc_init = false;
    config->wakenet_init = true;
    config->wakenet_model_name = WAKE_WORD_MODEL_NAME;
    config->wakenet_model_name_2 = NULL;
    /* ESP-SR creates its own internal `afe_mase` consumer. It must outrank
     * the priority-5 feed/fetch bridge workers so the two-frame AFE input ring
     * is drained before a normal worker time slice can refill it. It remains
     * below the priority-7 I2S owner. */
    config->afe_perferred_core = WAKE_WORD_AFE_ENGINE_CORE;
    config->afe_perferred_priority = WAKE_WORD_AFE_ENGINE_PRIORITY;
    config->afe_ringbuf_size = 2;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE;
    s_runtime.afe_iface = esp_afe_handle_from_config(config);
    s_runtime.afe = (s_runtime.afe_iface == NULL) ? NULL :
        s_runtime.afe_iface->create_from_config(config);
    afe_config_free(config);
    if (s_runtime.afe == NULL) {
        s_runtime.status.last_error = ESP_FAIL;
        return ESP_FAIL;
    }
    const int feed_samples_per_channel =
        s_runtime.afe_iface->get_feed_chunksize(s_runtime.afe);
    const int fetch_samples =
        s_runtime.afe_iface->get_fetch_chunksize(s_runtime.afe);
    const int feed_channels =
        s_runtime.afe_iface->get_feed_channel_num(s_runtime.afe);
    const int fetch_channels =
        s_runtime.afe_iface->get_fetch_channel_num(s_runtime.afe);
    const size_t feed_samples_total =
        ((feed_samples_per_channel > 0) && (feed_channels > 0))
            ? ((size_t)feed_samples_per_channel * (size_t)feed_channels)
            : 0U;
    if ((s_runtime.afe_iface->get_samp_rate(s_runtime.afe) !=
         (int)AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ) ||
        (feed_channels != (int)AUDIO_MANAGER_STREAM_CHANNELS) ||
        (fetch_channels != 1) ||
        (feed_samples_per_channel <= 0) ||
        (fetch_samples <= 0) ||
        (feed_samples_total == 0U) ||
        (feed_samples_total > WAKE_WORD_MAX_FEED_SAMPLES)) {
        s_runtime.afe_iface->destroy(s_runtime.afe);
        s_runtime.afe = NULL;
        s_runtime.status.last_error = ESP_ERR_INVALID_SIZE;
        return ESP_ERR_INVALID_SIZE;
    }
    s_runtime.feed_samples = feed_samples_total;
    s_runtime.status.afe_feed_samples_per_channel =
        (uint32_t)feed_samples_per_channel;
    s_runtime.status.afe_fetch_samples = (uint32_t)fetch_samples;
    s_runtime.status.afe_feed_channels = (uint8_t)feed_channels;
    s_runtime.status.afe_fetch_channels = (uint8_t)fetch_channels;
    const size_t largest_internal_after_afe = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_runtime.status.internal_largest_after_afe_bytes =
        (largest_internal_after_afe > UINT32_MAX) ? UINT32_MAX :
        (uint32_t)largest_internal_after_afe;
    s_runtime.status.initialized = true;
    s_runtime.status.detector_ready = true;
    s_runtime.status.state = WAKE_WORD_STATE_DISABLED;
    s_runtime.status.last_error = ESP_OK;
    APP_LOGI(TAG, AFE_GEOMETRY_VERIFIED_470AFDA8,
             "AFE geometry: rate=%u feed=%u x %u fetch=%u x %u ring=%u internal_largest=%u->%u",
             (unsigned)AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ,
             (unsigned)s_runtime.status.afe_feed_samples_per_channel,
             (unsigned)s_runtime.status.afe_feed_channels,
             (unsigned)s_runtime.status.afe_fetch_samples,
             (unsigned)s_runtime.status.afe_fetch_channels,
             2U,
             (unsigned)s_runtime.status.internal_largest_before_afe_bytes,
             (unsigned)s_runtime.status.internal_largest_after_afe_bytes);
    return ESP_OK;
}

esp_err_t wake_word_manager_start(void)
{
    if (!s_runtime.status.initialized || (s_runtime.feed_task != NULL) ||
        (s_runtime.fetch_task != NULL)) {
        return s_runtime.status.initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = audio_manager_stream_local_monitor_register();
    if (ret != ESP_OK) {
        return ret;
    }
    /* These workers handle copied PCM and AFE results only: their stacks are
     * PSRAM-safe. Preserve scarce Internal RAM for ESP-SR's private engine,
     * I2S/DMA, and Xiaozhi transport task creation. */
    if (xTaskCreateWithCaps(wake_word_fetch_task, WAKE_WORD_FETCH_TASK_NAME,
                            WAKE_WORD_TASK_STACK, NULL, WAKE_WORD_TASK_PRIORITY,
                            &s_runtime.fetch_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_runtime.fetch_task = NULL;
        (void)audio_manager_stream_local_monitor_unregister();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreateWithCaps(wake_word_feed_task, WAKE_WORD_FEED_TASK_NAME,
                            WAKE_WORD_TASK_STACK, NULL, WAKE_WORD_TASK_PRIORITY,
                            &s_runtime.feed_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vTaskDeleteWithCaps(s_runtime.fetch_task);
        s_runtime.fetch_task = NULL;
        s_runtime.feed_task = NULL;
        (void)audio_manager_stream_local_monitor_unregister();
        return ESP_ERR_NO_MEM;
    }
    s_runtime.status.running = true;
    return ESP_OK;
}

static void wake_word_set_capture_requested_status(bool requested)
{
    if (wake_word_take_lock()) {
        s_runtime.status.capture_requested = requested;
        xSemaphoreGive(s_runtime.lock);
    }
}

static esp_err_t wake_word_submit_capture_request(void)
{
    audio_manager_request_t request = {0};
    esp_err_t ret = audio_manager_request_make_default(
        WAKE_WORD_REQUEST_ID,
        AUDIO_MANAGER_CLIENT_WAKE_WORD,
        AUDIO_MANAGER_RESOURCE_CAPTURE,
        &request);
    if (ret != ESP_OK) {
        return ret;
    }
    request.busy_policy = AUDIO_MANAGER_BUSY_REJECT;
    request.priority = AUDIO_MANAGER_PRIORITY_BACKGROUND;
    request.interruptible = true;
    ret = audio_manager_capture_arbiter_submit(&request);
    if (ret == ESP_OK) {
        wake_word_set_capture_requested_status(true);
    }
    return ret;
}

static bool wake_word_capture_request_is_present(
    const audio_manager_capture_arbiter_status_t *arbiter)
{
    return (arbiter != NULL) &&
           ((arbiter->current_valid &&
             (arbiter->current.request_id == WAKE_WORD_REQUEST_ID)) ||
            (arbiter->pending_valid &&
             (arbiter->pending.request_id == WAKE_WORD_REQUEST_ID)));
}

/* The capture arbiter intentionally discards a low-priority monitor request
 * when Xiaozhi preempts it. Poll its copied state at most four times per
 * second so the monitor is re-submitted after the voice/TTS turn, without a
 * second I2S reader or a queue behind live capture. */
static void wake_word_ensure_capture_request(int64_t now_us)
{
    if (!wake_word_take_lock()) {
        return;
    }
    const bool should_request = s_runtime.status.enabled && !s_runtime.status.muted;
    if (!should_request || (now_us < s_runtime.next_capture_check_us)) {
        xSemaphoreGive(s_runtime.lock);
        return;
    }
    s_runtime.next_capture_check_us = now_us + 250000LL;
    xSemaphoreGive(s_runtime.lock);

    audio_manager_capture_arbiter_status_t arbiter = {0};
    if (audio_manager_capture_arbiter_get_status(&arbiter) != ESP_OK) {
        return;
    }
    if (wake_word_capture_request_is_present(&arbiter)) {
        wake_word_set_capture_requested_status(true);
        return;
    }
    wake_word_set_capture_requested_status(false);

    if (voice_recording_critical_is_active() || wake_word_speaker_is_active()) {
        return;
    }
    (void)wake_word_submit_capture_request();
}

esp_err_t wake_word_manager_set_enabled(bool enabled)
{
    if (!s_runtime.status.running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled) {
        esp_err_t ret = audio_manager_stream_local_monitor_set_enabled(true);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = wake_word_submit_capture_request();
        if (ret != ESP_OK) {
            (void)audio_manager_stream_local_monitor_set_enabled(false);
            return ret;
        }
        if (wake_word_take_lock()) {
            s_runtime.status.enabled = true;
            s_runtime.status.muted = false;
            s_runtime.status.capture_requested = true;
            s_runtime.status.handsfree_handoff_active = false;
            s_runtime.next_capture_check_us = 0;
            s_runtime.status.state = wake_word_runtime_next_state(
                s_runtime.status.state, WAKE_WORD_RUNTIME_TRIGGER_ENABLE);
            s_runtime.status.last_transition_us = (uint64_t)esp_timer_get_time();
            xSemaphoreGive(s_runtime.lock);
        }
        wake_word_request_afe_reset();
        wake_word_set_afe_processing_active(true);
        return ESP_OK;
    }
    (void)audio_manager_capture_arbiter_cancel(WAKE_WORD_REQUEST_ID);
    atomic_store_explicit(&s_runtime.handsfree_handoff_active, false,
                          memory_order_release);
    (void)audio_manager_stream_local_monitor_set_allow_during_ptt(false);
    (void)audio_manager_stream_local_monitor_set_enabled(false);
    if (wake_word_take_lock()) {
        const bool active = (s_runtime.status.state == WAKE_WORD_STATE_WAIT_FOR_SPEECH) ||
                            (s_runtime.status.state == WAKE_WORD_STATE_LISTENING);
        s_runtime.status.enabled = false;
        s_runtime.status.muted = false;
        s_runtime.status.capture_requested = false;
        s_runtime.status.handsfree_handoff_active = false;
        s_runtime.status.state = wake_word_runtime_next_state(
            s_runtime.status.state, WAKE_WORD_RUNTIME_TRIGGER_DISABLE);
        s_runtime.rearm_after_us = 0;
        s_runtime.status.last_transition_us = (uint64_t)esp_timer_get_time();
        if (active) {
            ++s_runtime.status.cancel_count;
        }
        xSemaphoreGive(s_runtime.lock);
        if (active) {
            wake_word_emit_event(WAKE_WORD_EVENT_CANCELLED, 0U, 0U);
        }
    }
    wake_word_request_afe_reset();
    return ESP_OK;
}

esp_err_t wake_word_manager_set_muted(bool muted)
{
    if (!s_runtime.status.running || !wake_word_take_lock()) {
        return s_runtime.status.running ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }

    if (s_runtime.status.muted == muted) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_OK;
    }

    const int64_t now_us = esp_timer_get_time();
    const wake_word_manager_state_t previous = s_runtime.status.state;
    const uint32_t generation = s_runtime.status.wake_generation;
    const bool cancel = muted && ((previous == WAKE_WORD_STATE_WAIT_FOR_SPEECH) ||
                                  (previous == WAKE_WORD_STATE_LISTENING));
    s_runtime.status.muted = muted;
    if (muted) {
        s_runtime.status.handsfree_handoff_active = false;
    }
    s_runtime.status.last_transition_us = (uint64_t)now_us;
    if (muted) {
        s_runtime.status.state = wake_word_runtime_next_state(
            previous, WAKE_WORD_RUNTIME_TRIGGER_MUTE);
        if (cancel) {
            ++s_runtime.status.cancel_count;
            ++s_runtime.status.mute_cancel_count;
        }
    } else if (s_runtime.status.enabled) {
        s_runtime.status.state = wake_word_runtime_next_state(
            previous, WAKE_WORD_RUNTIME_TRIGGER_UNMUTE);
        s_runtime.rearm_after_us = now_us + ((int64_t)WAKE_WORD_REARM_GUARD_MS * 1000LL);
    }
    xSemaphoreGive(s_runtime.lock);

    if (muted) {
        atomic_store_explicit(&s_runtime.handsfree_handoff_active, false,
                              memory_order_release);
        (void)audio_manager_stream_local_monitor_set_allow_during_ptt(false);
    }

    /* Muting leaves audio_manager's I2S/DMA ownership untouched. The worker
     * drops local processing and the AFE is reset so an old utterance cannot
     * resume after unmute. */
    wake_word_request_afe_reset();
    APP_LOGI(TAG, WAKE_MUTE_G_U_30A46F57,
             "WAKE state %u -> %s gen=%lu reason=mute",
             (unsigned)previous,
             muted ? "DISABLED" : "REARM_GUARD",
             (unsigned long)generation);
    if (cancel) {
        wake_word_emit_event(WAKE_WORD_EVENT_CANCELLED, 0U, 0U);
    }
    return ESP_OK;
}

esp_err_t wake_word_manager_set_handsfree_handoff_active(bool active)
{
    if (!s_runtime.status.running) {
        return ESP_ERR_INVALID_STATE;
    }

    if (active) {
        if (!wake_word_take_lock()) {
            return ESP_ERR_TIMEOUT;
        }
        const bool valid = s_runtime.status.enabled && !s_runtime.status.muted &&
            ((s_runtime.status.state == WAKE_WORD_STATE_WAIT_FOR_SPEECH) ||
             (s_runtime.status.state == WAKE_WORD_STATE_LISTENING));
        if (!valid) {
            xSemaphoreGive(s_runtime.lock);
            return ESP_ERR_INVALID_STATE;
        }
        s_runtime.status.handsfree_handoff_active = true;
        xSemaphoreGive(s_runtime.lock);
        atomic_store_explicit(&s_runtime.handsfree_handoff_active, true,
                              memory_order_release);
        const esp_err_t ret =
            audio_manager_stream_local_monitor_set_allow_during_ptt(true);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        atomic_store_explicit(&s_runtime.handsfree_handoff_active, false,
                              memory_order_release);
        if (wake_word_take_lock()) {
            s_runtime.status.handsfree_handoff_active = false;
            xSemaphoreGive(s_runtime.lock);
        }
        return ret;
    }

    const bool was_active = atomic_exchange_explicit(
        &s_runtime.handsfree_handoff_active, false, memory_order_acq_rel);
    const esp_err_t monitor_ret =
        audio_manager_stream_local_monitor_set_allow_during_ptt(false);
    bool cancel_local_turn = false;
    if (wake_word_take_lock()) {
        cancel_local_turn = was_active &&
            ((s_runtime.status.state == WAKE_WORD_STATE_WAIT_FOR_SPEECH) ||
             (s_runtime.status.state == WAKE_WORD_STATE_LISTENING));
        s_runtime.status.handsfree_handoff_active = false;
        xSemaphoreGive(s_runtime.lock);
    }
    if (cancel_local_turn) {
        wake_word_enter_rearm(WAKE_WORD_EVENT_CANCELLED, 0U,
                              esp_timer_get_time(), false);
    } else if (was_active) {
        wake_word_request_afe_reset();
    }
    return (monitor_ret == ESP_ERR_INVALID_STATE) ? ESP_OK : monitor_ret;
}

esp_err_t wake_word_manager_get_status(wake_word_manager_status_t *status)
{
    if (status == NULL || !wake_word_take_lock()) {
        return (status == NULL) ? ESP_ERR_INVALID_ARG : ESP_ERR_TIMEOUT;
    }
    *status = s_runtime.status;
    xSemaphoreGive(s_runtime.lock);
    status->afe_input_ready = atomic_load_explicit(&s_runtime.afe_input_ready,
                                                   memory_order_acquire);
    status->handsfree_handoff_active = atomic_load_explicit(
        &s_runtime.handsfree_handoff_active, memory_order_acquire);
    portENTER_CRITICAL(&s_afe_metrics_lock);
    status->pcm_frames_received = s_afe_metrics.pcm_frames_received;
    status->afe_feed_calls = s_afe_metrics.afe_feed_calls;
    status->afe_fetch_calls = s_afe_metrics.afe_fetch_calls;
    status->afe_feed_failures = s_afe_metrics.afe_feed_failures;
    status->afe_fetch_failures = s_afe_metrics.afe_fetch_failures;
    status->afe_feed_dropped_reset = s_afe_metrics.afe_feed_dropped_reset;
    status->max_afe_feed_us = s_afe_metrics.max_afe_feed_us;
    status->max_afe_fetch_us = s_afe_metrics.max_afe_fetch_us;
    portEXIT_CRITICAL(&s_afe_metrics_lock);

    if (s_runtime.feed_task != NULL) {
        status->feed_task_stack_high_water_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(s_runtime.feed_task);
    }
    if (s_runtime.fetch_task != NULL) {
        status->fetch_task_stack_high_water_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(s_runtime.fetch_task);
    }
    audio_manager_stream_status_t stream = {0};
    if (audio_manager_stream_get_status(&stream) == ESP_OK) {
        status->wake_pcm_queue_peak_depth =
            stream.local_monitor.queue_peak_depth;
        status->wake_pcm_queue_drops =
            stream.local_monitor.frames_dropped_queue_full;
    }
    return ESP_OK;
}

esp_err_t wake_word_manager_register_callback(wake_word_manager_event_callback_t callback,
                                              void *user_context)
{
    if (!wake_word_take_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_runtime.callback = callback;
    s_runtime.callback_context = user_context;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

#else

esp_err_t wake_word_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wake_word_manager_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wake_word_manager_set_enabled(bool enabled) { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wake_word_manager_set_muted(bool muted) { (void)muted; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wake_word_manager_set_handsfree_handoff_active(bool active)
{ (void)active; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wake_word_manager_get_status(wake_word_manager_status_t *status)
{ if (status == NULL) return ESP_ERR_INVALID_ARG; *status = (wake_word_manager_status_t){0}; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t wake_word_manager_register_callback(wake_word_manager_event_callback_t callback, void *user_context)
{ (void)callback; (void)user_context; return ESP_ERR_NOT_SUPPORTED; }

#endif
