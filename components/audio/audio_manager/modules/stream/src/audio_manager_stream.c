#include "audio_manager_stream.h"
#include "audio_manager_stream_internal.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

static portMUX_TYPE s_stream_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_manager_stream_frame_callback_t s_stream_callback = NULL;
static void *s_stream_callback_context = NULL;
static audio_manager_stream_status_t s_stream_status = {0};

/* This is deliberately one fixed future consumer, not a generic subscriber
 * registry. Its copied payload is PSRAM-backed; the queue control object and
 * synchronization stay in normal memory. */
static QueueHandle_t s_local_monitor_queue = NULL;
static StaticQueue_t s_local_monitor_queue_control = {0};
static uint8_t *s_local_monitor_queue_storage = NULL;
static bool s_local_monitor_allocation_in_progress = false;
static uint64_t s_local_monitor_last_received_sequence = 0U;
/* Called only by the audio-manager producer task. Keeping the transient copy
 * static avoids adding a > 500-byte object to that task's hot-path stack. */
static audio_manager_stream_local_monitor_frame_t s_local_monitor_publish_frame;

_Static_assert(sizeof(audio_manager_stream_local_monitor_frame_t) == 528U,
               "local monitor queue frame must remain a compact fixed copy");

static void local_monitor_reset_status_locked(bool registered,
                                              bool enabled,
                                              bool allow_during_ptt)
{
    s_stream_status.local_monitor = (audio_manager_stream_local_monitor_status_t) {
        .registered = registered,
        .enabled = enabled,
        .allow_during_ptt = allow_during_ptt,
    };
    s_local_monitor_last_received_sequence = 0U;
}

static void local_monitor_publish(
    const audio_manager_stream_frame_t *source,
    bool ptt_armed)
{
    QueueHandle_t queue = NULL;
    uint64_t sequence = 0U;

    portENTER_CRITICAL(&s_stream_lock);
    if (!s_stream_status.local_monitor.registered ||
        !s_stream_status.local_monitor.enabled) {
        portEXIT_CRITICAL(&s_stream_lock);
        return;
    }
    if (ptt_armed && !s_stream_status.local_monitor.allow_during_ptt) {
        ++s_stream_status.local_monitor.frames_suppressed_ptt;
        portEXIT_CRITICAL(&s_stream_lock);
        return;
    }

    queue = s_local_monitor_queue;
    sequence = ++s_stream_status.local_monitor.frames_published;
    portEXIT_CRITICAL(&s_stream_lock);

    if (queue == NULL) {
        return;
    }

    s_local_monitor_publish_frame.sample_rate_hz = source->sample_rate_hz;
    s_local_monitor_publish_frame.channels = source->channels;
    s_local_monitor_publish_frame.sample_count = (uint16_t)source->sample_count;
    s_local_monitor_publish_frame.frame_sequence = sequence;
    memcpy(s_local_monitor_publish_frame.samples,
           source->samples,
           source->sample_count * sizeof(source->samples[0]));

    if (xQueueSend(queue, &s_local_monitor_publish_frame, 0U) != pdTRUE) {
        portENTER_CRITICAL(&s_stream_lock);
        ++s_stream_status.local_monitor.frames_dropped_queue_full;
        portEXIT_CRITICAL(&s_stream_lock);
        return;
    }

    const UBaseType_t queue_depth = uxQueueMessagesWaiting(queue);
    portENTER_CRITICAL(&s_stream_lock);
    ++s_stream_status.local_monitor.frames_delivered;
    if ((uint32_t)queue_depth >
        s_stream_status.local_monitor.queue_peak_depth) {
        s_stream_status.local_monitor.queue_peak_depth =
            (uint32_t)queue_depth;
    }
    portEXIT_CRITICAL(&s_stream_lock);
}

esp_err_t audio_manager_stream_register_callback(
    audio_manager_stream_frame_callback_t callback,
    void *user_context)
{
    portENTER_CRITICAL(&s_stream_lock);
    s_stream_callback = callback;
    s_stream_callback_context = user_context;
    portEXIT_CRITICAL(&s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_arm(uint32_t stream_generation)
{
    if (stream_generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_stream_lock);
    if (s_stream_status.armed)
    {
        portEXIT_CRITICAL(&s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const audio_manager_stream_local_monitor_status_t local_monitor =
        s_stream_status.local_monitor;
    memset(&s_stream_status, 0, sizeof(s_stream_status));
    s_stream_status.armed = true;
    s_stream_status.stream_generation = stream_generation;
    s_stream_status.local_monitor = local_monitor;
    portEXIT_CRITICAL(&s_stream_lock);

    audio_manager_stream_tap_arm();
    return ESP_OK;
}

esp_err_t audio_manager_stream_disarm(uint32_t stream_generation)
{
    if (stream_generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_stream_lock);
    if (!s_stream_status.armed ||
        (s_stream_status.stream_generation != stream_generation))
    {
        portEXIT_CRITICAL(&s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_stream_status.armed = false;
    portEXIT_CRITICAL(&s_stream_lock);

    audio_manager_stream_tap_disarm();
    return ESP_OK;
}

esp_err_t audio_manager_stream_get_status(
    audio_manager_stream_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_stream_lock);
    *status = s_stream_status;
    portEXIT_CRITICAL(&s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_local_monitor_register(void)
{
    portENTER_CRITICAL(&s_stream_lock);
    if (s_stream_status.local_monitor.registered ||
        s_local_monitor_allocation_in_progress) {
        portEXIT_CRITICAL(&s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_local_monitor_allocation_in_progress = true;
    portEXIT_CRITICAL(&s_stream_lock);

    const size_t storage_bytes =
        (size_t)AUDIO_MANAGER_STREAM_LOCAL_MONITOR_QUEUE_LENGTH *
        sizeof(audio_manager_stream_local_monitor_frame_t);
    if (s_local_monitor_queue_storage == NULL) {
        s_local_monitor_queue_storage = heap_caps_malloc(
            storage_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_local_monitor_queue_storage == NULL) {
            portENTER_CRITICAL(&s_stream_lock);
            s_local_monitor_allocation_in_progress = false;
            portEXIT_CRITICAL(&s_stream_lock);
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_local_monitor_queue == NULL) {
        s_local_monitor_queue = xQueueCreateStatic(
            AUDIO_MANAGER_STREAM_LOCAL_MONITOR_QUEUE_LENGTH,
            sizeof(audio_manager_stream_local_monitor_frame_t),
            s_local_monitor_queue_storage,
            &s_local_monitor_queue_control);
        if (s_local_monitor_queue == NULL) {
            portENTER_CRITICAL(&s_stream_lock);
            s_local_monitor_allocation_in_progress = false;
            portEXIT_CRITICAL(&s_stream_lock);
            return ESP_ERR_NO_MEM;
        }
    }

    (void)xQueueReset(s_local_monitor_queue);
    portENTER_CRITICAL(&s_stream_lock);
    s_local_monitor_allocation_in_progress = false;
    local_monitor_reset_status_locked(true, false, false);
    portEXIT_CRITICAL(&s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_local_monitor_unregister(void)
{
    portENTER_CRITICAL(&s_stream_lock);
    const bool registered = s_stream_status.local_monitor.registered;
    const bool active = s_stream_status.local_monitor.enabled ||
                        s_stream_status.armed;
    portEXIT_CRITICAL(&s_stream_lock);
    if (!registered || active) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_local_monitor_queue != NULL) {
        (void)xQueueReset(s_local_monitor_queue);
    }
    portENTER_CRITICAL(&s_stream_lock);
    local_monitor_reset_status_locked(false, false, false);
    portEXIT_CRITICAL(&s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_local_monitor_set_enabled(bool enabled)
{
    QueueHandle_t queue = NULL;
    bool changed = false;
    bool allow_during_ptt = false;

    portENTER_CRITICAL(&s_stream_lock);
    if (!s_stream_status.local_monitor.registered) {
        portEXIT_CRITICAL(&s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }
    changed = s_stream_status.local_monitor.enabled != enabled;
    allow_during_ptt = s_stream_status.local_monitor.allow_during_ptt;
    queue = s_local_monitor_queue;
    portEXIT_CRITICAL(&s_stream_lock);

    if (!changed) {
        return ESP_OK;
    }
    if (queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xQueueReset(queue);
    portENTER_CRITICAL(&s_stream_lock);
    local_monitor_reset_status_locked(true, enabled,
                                      enabled && allow_during_ptt);
    portEXIT_CRITICAL(&s_stream_lock);
    audio_manager_stream_tap_set_local_monitor_enabled(enabled);
    return ESP_OK;
}

esp_err_t audio_manager_stream_local_monitor_set_allow_during_ptt(bool allowed)
{
    portENTER_CRITICAL(&s_stream_lock);
    if (!s_stream_status.local_monitor.registered ||
        (allowed && !s_stream_status.local_monitor.enabled)) {
        portEXIT_CRITICAL(&s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_stream_status.local_monitor.allow_during_ptt = allowed;
    portEXIT_CRITICAL(&s_stream_lock);
    return ESP_OK;
}


esp_err_t audio_manager_stream_local_monitor_receive(
    audio_manager_stream_local_monitor_frame_t *frame,
    uint32_t timeout_ms)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    QueueHandle_t queue = NULL;
    portENTER_CRITICAL(&s_stream_lock);
    if (!s_stream_status.local_monitor.registered ||
        !s_stream_status.local_monitor.enabled) {
        portEXIT_CRITICAL(&s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }
    queue = s_local_monitor_queue;
    portEXIT_CRITICAL(&s_stream_lock);
    if (queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueReceive(queue, frame, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_stream_lock);
    if ((s_local_monitor_last_received_sequence != 0U) &&
        (frame->frame_sequence != (s_local_monitor_last_received_sequence + 1U))) {
        ++s_stream_status.local_monitor.sequence_gap_count;
    }
    s_local_monitor_last_received_sequence = frame->frame_sequence;
    portEXIT_CRITICAL(&s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_publish_internal(
    const int16_t *samples,
    size_t sample_count)
{
    if ((samples == NULL) ||
        (sample_count == 0U) ||
        (sample_count > AUDIO_MANAGER_STREAM_FRAME_SAMPLES))
    {
        return ESP_ERR_INVALID_ARG;
    }

    audio_manager_stream_frame_callback_t callback = NULL;
    void *callback_context = NULL;
    audio_manager_stream_frame_t frame = {0};
    bool ptt_armed = false;

    portENTER_CRITICAL(&s_stream_lock);
    ptt_armed = s_stream_status.armed;
    if (ptt_armed) {
        frame.samples = samples;
        frame.sample_count = sample_count;
        frame.sample_rate_hz = AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ;
        frame.channels = AUDIO_MANAGER_STREAM_CHANNELS;
        frame.stream_generation = s_stream_status.stream_generation;
        frame.frame_sequence = s_stream_status.frames_published + 1U;

        callback = s_stream_callback;
        callback_context = s_stream_callback_context;

        ++s_stream_status.frames_published;
        s_stream_status.samples_published += sample_count;
        if (callback == NULL) {
            ++s_stream_status.frames_dropped_no_callback;
        }
    } else {
        frame.samples = samples;
        frame.sample_count = sample_count;
        frame.sample_rate_hz = AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ;
        frame.channels = AUDIO_MANAGER_STREAM_CHANNELS;
    }
    portEXIT_CRITICAL(&s_stream_lock);

    /* Existing PTT remains first and follows its established borrowed-frame,
     * zero-wait queue-copy contract. The local queue is independent below. */
    if (callback != NULL) {
        callback(&frame, callback_context);
    }
    local_monitor_publish(&frame, ptt_armed);
    return ESP_OK;
}
