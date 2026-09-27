/**
 * @file afe_pipeline.c
 * @brief Continuously-drained ESP-SR AFE proof with WakeNet/VAD observations.
 */

#include "afe_pipeline.h"

#include "sdkconfig.h"

#include <string.h>

#if CONFIG_AFE_PIPELINE_ENABLE

#include "audio_manager.h"
#include "audio_manager_stream.h"
#include "app_log.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "model_path.h"
#include "performance_monitor.h"

#include <limits.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AFE_PIPELINE_PCM_FRAME_SAMPLES       256U
#define AFE_PIPELINE_PCM_QUEUE_LENGTH           4U
#define AFE_PIPELINE_WAKE_EVENT_QUEUE_LENGTH    4U
#define AFE_PIPELINE_MAX_FEED_SAMPLES         1024U
#define AFE_PIPELINE_TASK_PRIORITY               5U
#define AFE_PIPELINE_TASK_STACK_BYTES         6144U
#define AFE_PIPELINE_DETECTION_TASK_PRIORITY     4U
#define AFE_PIPELINE_DETECTION_TASK_STACK_BYTES 3072U
#define AFE_PIPELINE_SUMMARY_PERIOD_US   (60000000LL)

static char s_wakenet_model_name[] = "wn9_hiesp";

typedef struct
{
    uint16_t sample_count;
    int16_t samples[AFE_PIPELINE_PCM_FRAME_SAMPLES];
} afe_pipeline_pcm_frame_t;

typedef struct
{
    const esp_afe_sr_iface_t *iface;
    esp_afe_sr_data_t *afe;
    srmodel_list_t *models;
    QueueHandle_t pcm_queue;
    QueueHandle_t wake_event_queue;
    TaskHandle_t feed_task;
    TaskHandle_t fetch_task;
    TaskHandle_t detection_task;
    bool started;
    int feed_samples_per_channel;
    int fetch_samples;
    int feed_channels;
    int fetch_channels;
    afe_pipeline_status_t status;
} afe_pipeline_runtime_t;

static const char *TAG = "afe_pipeline";
static afe_pipeline_runtime_t s_runtime = {0};
static portMUX_TYPE s_metrics_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t afe_pipeline_to_u32(size_t value)
{
    return (value > UINT32_MAX) ? UINT32_MAX : (uint32_t)value;
}

static uint32_t afe_pipeline_duration_to_u32(int64_t duration_us)
{
    if (duration_us <= 0) {
        return 0U;
    }
    return ((uint64_t)duration_us > UINT32_MAX) ?
        UINT32_MAX : (uint32_t)duration_us;
}

static void afe_pipeline_take_heap_snapshot(
    uint32_t capabilities,
    afe_pipeline_heap_snapshot_t *snapshot)
{
    multi_heap_info_t info = {0};
    heap_caps_get_info(&info, capabilities);
    snapshot->free_bytes = afe_pipeline_to_u32(info.total_free_bytes);
    snapshot->minimum_free_bytes = afe_pipeline_to_u32(info.minimum_free_bytes);
    snapshot->largest_free_block_bytes =
        afe_pipeline_to_u32(info.largest_free_block);
}

static void afe_pipeline_take_cpu_snapshot(afe_pipeline_cpu_snapshot_t *snapshot)
{
    performance_monitor_status_t latest = {0};
    if (performance_monitor_get_status(&latest) != ESP_OK) {
        memset(snapshot, 0, sizeof(*snapshot));
        return;
    }

    snapshot->sample_valid = latest.sample_valid;
    snapshot->cpu_used_x10 = latest.cpu_used_x10;
    snapshot->cpu_peak_500ms_x10 = latest.cpu_peak_500ms_x10;
    snapshot->cpu_idle_x10 = latest.cpu_idle_x10;
    snapshot->captured_at_us = latest.captured_at_us;
}

static void afe_pipeline_take_current_resources(afe_pipeline_status_t *status)
{
    afe_pipeline_take_heap_snapshot(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, &status->current_internal);
    afe_pipeline_take_heap_snapshot(
        MALLOC_CAP_DMA | MALLOC_CAP_8BIT, &status->current_dma);
    afe_pipeline_take_heap_snapshot(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, &status->current_psram);
    afe_pipeline_take_cpu_snapshot(&status->current_cpu);
    status->task_count_current = (uint32_t)uxTaskGetNumberOfTasks();
}

static void afe_pipeline_note_pcm_queue(bool queued)
{
    uint32_t queued_now = 0U;
    if (queued && (s_runtime.pcm_queue != NULL)) {
        queued_now = (uint32_t)uxQueueMessagesWaiting(s_runtime.pcm_queue);
    }

    portENTER_CRITICAL(&s_metrics_lock);
    ++s_runtime.status.pcm_frames_received;
    if (!queued) {
        ++s_runtime.status.pcm_queue_drops;
    } else {
        if (queued_now > s_runtime.status.pcm_queue_peak) {
            s_runtime.status.pcm_queue_peak = queued_now;
        }
    }
    portEXIT_CRITICAL(&s_metrics_lock);
}

static void afe_pipeline_note_feed(int result, uint32_t duration_us)
{
    portENTER_CRITICAL(&s_metrics_lock);
    ++s_runtime.status.feed_attempts;
    if (duration_us > s_runtime.status.max_feed_us) {
        s_runtime.status.max_feed_us = duration_us;
    }
    if (result > 0) {
        ++s_runtime.status.feed_accepted;
    } else if (result == 0) {
        ++s_runtime.status.feed_rejected;
    } else {
        ++s_runtime.status.feed_errors;
        s_runtime.status.last_error = ESP_FAIL;
    }
    portEXIT_CRITICAL(&s_metrics_lock);
}

static void afe_pipeline_note_fetch(
    const afe_fetch_result_t *result,
    uint32_t duration_us)
{
    portENTER_CRITICAL(&s_metrics_lock);
    ++s_runtime.status.fetch_calls;
    if (duration_us > s_runtime.status.max_fetch_us) {
        s_runtime.status.max_fetch_us = duration_us;
    }
    if (result == NULL) {
        ++s_runtime.status.fetch_errors;
        s_runtime.status.last_error = ESP_FAIL;
    } else if (result->ret_value == ESP_OK) {
        ++s_runtime.status.fetch_success;
    } else if ((result->data == NULL) && (result->data_size == 0)) {
        /* ESP-SR 2.4.7 1MIC reports its no-output fetch timeout this way. */
        ++s_runtime.status.fetch_timeout;
    } else {
        ++s_runtime.status.fetch_errors;
        s_runtime.status.last_error = ESP_FAIL;
    }
    portEXIT_CRITICAL(&s_metrics_lock);
}

static void afe_pipeline_note_vad(const afe_fetch_result_t *result)
{
    if ((result == NULL) || (result->ret_value != ESP_OK)) {
        return;
    }

    const bool speech = result->vad_state == VAD_SPEECH;
    portENTER_CRITICAL(&s_metrics_lock);
    if (speech) {
        ++s_runtime.status.vad_speech_results;
    } else {
        ++s_runtime.status.vad_non_speech_results;
    }
    if (s_runtime.status.vad_state_valid &&
        (s_runtime.status.vad_speech != speech)) {
        ++s_runtime.status.vad_transitions;
    }
    s_runtime.status.vad_state_valid = true;
    s_runtime.status.vad_speech = speech;
    portEXIT_CRITICAL(&s_metrics_lock);
}

static void afe_pipeline_enqueue_wake_event(const afe_fetch_result_t *result)
{
    if ((result == NULL) || (result->ret_value != ESP_OK) ||
        (result->wakeup_state != WAKENET_DETECTED)) {
        return;
    }

    afe_pipeline_wake_event_t event = {
        .detected_at_us = esp_timer_get_time(),
        .wake_word_index = result->wake_word_index,
        .wakenet_model_index = result->wakenet_model_index,
    };
    QueueHandle_t wake_event_queue = NULL;

    portENTER_CRITICAL(&s_metrics_lock);
    ++s_runtime.status.wake_detections;
    event.event_sequence = s_runtime.status.wake_detections;
    event.fetch_sequence = s_runtime.status.fetch_success;
    s_runtime.status.last_wake_timestamp_us = event.detected_at_us;
    s_runtime.status.last_wake_word_index = event.wake_word_index;
    s_runtime.status.last_wakenet_model_index = event.wakenet_model_index;
    wake_event_queue = s_runtime.wake_event_queue;
    portEXIT_CRITICAL(&s_metrics_lock);

    const bool queued = (wake_event_queue != NULL) &&
        (xQueueSend(wake_event_queue, &event, 0U) == pdPASS);
    const uint32_t queued_now = queued ?
        (uint32_t)uxQueueMessagesWaiting(wake_event_queue) : 0U;

    portENTER_CRITICAL(&s_metrics_lock);
    if (queued) {
        ++s_runtime.status.wake_events_enqueued;
        if (queued_now > s_runtime.status.wake_event_queue_peak) {
            s_runtime.status.wake_event_queue_peak = queued_now;
        }
    } else {
        ++s_runtime.status.wake_events_dropped;
    }
    portEXIT_CRITICAL(&s_metrics_lock);
}

static void afe_pipeline_log_summary(void)
{
    afe_pipeline_status_t status = {0};
    if (afe_pipeline_get_status(&status) != ESP_OK) {
        return;
    }

    APP_LOGI(TAG, AFE_PIPELINE_SUMMARY_A34F07B2,
             "AFE summary pcm=%llu feed=%llu accepted=%llu rejected=%llu err=%llu "
             "fetch=%llu ok=%llu timeout=%llu err=%llu max_us=%u/%u queue=%u/%u drop=%llu",
             (unsigned long long)status.pcm_frames_received,
             (unsigned long long)status.feed_attempts,
             (unsigned long long)status.feed_accepted,
             (unsigned long long)status.feed_rejected,
             (unsigned long long)status.feed_errors,
             (unsigned long long)status.fetch_calls,
             (unsigned long long)status.fetch_success,
             (unsigned long long)status.fetch_timeout,
             (unsigned long long)status.fetch_errors,
             (unsigned int)status.max_feed_us,
             (unsigned int)status.max_fetch_us,
             (unsigned int)status.pcm_queue_current,
             (unsigned int)status.pcm_queue_peak,
             (unsigned long long)status.pcm_queue_drops);
    APP_LOGI(TAG, AFE_PIPELINE_RESOURCES_4BE75EAE,
             "AFE resources task=%u hwm_words=%u/%u internal=%u/%u/%u "
             "dma=%u/%u/%u psram=%u/%u/%u cpu=%s%u.%u%% peak=%u.%u%%",
             (unsigned int)status.task_count_current,
             (unsigned int)status.feed_task_stack_hwm_words,
             (unsigned int)status.fetch_task_stack_hwm_words,
             (unsigned int)status.current_internal.free_bytes,
             (unsigned int)status.current_internal.minimum_free_bytes,
             (unsigned int)status.current_internal.largest_free_block_bytes,
             (unsigned int)status.current_dma.free_bytes,
             (unsigned int)status.current_dma.minimum_free_bytes,
             (unsigned int)status.current_dma.largest_free_block_bytes,
             (unsigned int)status.current_psram.free_bytes,
             (unsigned int)status.current_psram.minimum_free_bytes,
             (unsigned int)status.current_psram.largest_free_block_bytes,
             status.current_cpu.sample_valid ? "" : "unavailable/",
             (unsigned int)(status.current_cpu.cpu_used_x10 / 10U),
             (unsigned int)(status.current_cpu.cpu_used_x10 % 10U),
             (unsigned int)(status.current_cpu.cpu_peak_500ms_x10 / 10U),
             (unsigned int)(status.current_cpu.cpu_peak_500ms_x10 % 10U));
    APP_LOGI(TAG, AFE_PIPELINE_DETECTION_SUMMARY_97A86CD0,
             "AFE detect wake=%llu event=%llu/%llu/%llu queue=%u/%u last=%lld/%d/%d "
             "vad=speech:%llu non_speech:%llu transitions:%llu state=%s",
             (unsigned long long)status.wake_detections,
             (unsigned long long)status.wake_events_enqueued,
             (unsigned long long)status.wake_events_dropped,
             (unsigned long long)status.wake_events_processed,
             (unsigned int)status.wake_event_queue_current,
             (unsigned int)status.wake_event_queue_peak,
             (long long)status.last_wake_timestamp_us,
             (int)status.last_wake_word_index,
             (int)status.last_wakenet_model_index,
             (unsigned long long)status.vad_speech_results,
             (unsigned long long)status.vad_non_speech_results,
             (unsigned long long)status.vad_transitions,
             !status.vad_state_valid ? "unavailable" :
                 (status.vad_speech ? "speech" : "non-speech"));
}

static void afe_pipeline_pcm_callback(
    const audio_manager_stream_frame_t *frame,
    void *user_context)
{
    (void)user_context;

    if ((frame == NULL) || (s_runtime.pcm_queue == NULL) ||
        (frame->samples == NULL) ||
        (frame->sample_rate_hz != AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ) ||
        (frame->channels != AUDIO_MANAGER_STREAM_CHANNELS) ||
        (frame->sample_count == 0U) ||
        (frame->sample_count > AFE_PIPELINE_PCM_FRAME_SAMPLES)) {
        afe_pipeline_note_pcm_queue(false);
        return;
    }

    afe_pipeline_pcm_frame_t copied = {
        .sample_count = (uint16_t)frame->sample_count,
    };
    memcpy(copied.samples, frame->samples,
           frame->sample_count * sizeof(copied.samples[0]));

    const bool queued = xQueueSend(s_runtime.pcm_queue, &copied, 0U) == pdPASS;
    afe_pipeline_note_pcm_queue(queued);
}

static void afe_pipeline_feed_task(void *argument)
{
    (void)argument;
    int16_t feed_buffer[AFE_PIPELINE_MAX_FEED_SAMPLES];
    size_t feed_fill = 0U;

    for (;;) {
        afe_pipeline_pcm_frame_t copied = {0};
        if (xQueueReceive(s_runtime.pcm_queue, &copied, portMAX_DELAY) != pdPASS) {
            continue;
        }

        const uint32_t queued_now =
            (uint32_t)uxQueueMessagesWaiting(s_runtime.pcm_queue);
        portENTER_CRITICAL(&s_metrics_lock);
        s_runtime.status.pcm_queue_current = queued_now;
        portEXIT_CRITICAL(&s_metrics_lock);

        size_t offset = 0U;
        while (offset < copied.sample_count) {
            const size_t required = (size_t)s_runtime.feed_samples_per_channel;
            const size_t available = required - feed_fill;
            const size_t remaining = (size_t)copied.sample_count - offset;
            const size_t to_copy = (remaining < available) ? remaining : available;
            memcpy(&feed_buffer[feed_fill], &copied.samples[offset],
                   to_copy * sizeof(feed_buffer[0]));
            feed_fill += to_copy;
            offset += to_copy;

            if (feed_fill != required) {
                continue;
            }

            int feed_result = 0;
            do {
                const int64_t started_us = esp_timer_get_time();
                feed_result = s_runtime.iface->feed(s_runtime.afe, feed_buffer);
                const uint32_t duration_us = afe_pipeline_duration_to_u32(
                    esp_timer_get_time() - started_us);
                afe_pipeline_note_feed(feed_result, duration_us);
                if (feed_result == 0) {
                    /* fetch() is independently running and drains the ring. */
                    vTaskDelay(1U);
                }
            } while (feed_result == 0);

            /* Negative return is a recorded failure; do not retry/reset in
             * Phase 24.2.1, and do not retain this invalid input indefinitely. */
            feed_fill = 0U;
        }
    }
}

static void afe_pipeline_fetch_task(void *argument)
{
    (void)argument;
    int64_t next_summary_us = esp_timer_get_time() + AFE_PIPELINE_SUMMARY_PERIOD_US;

    for (;;) {
        const int64_t started_us = esp_timer_get_time();
        afe_fetch_result_t *result = s_runtime.iface->fetch(s_runtime.afe);
        afe_pipeline_note_fetch(
            result, afe_pipeline_duration_to_u32(esp_timer_get_time() - started_us));
        afe_pipeline_note_vad(result);
        afe_pipeline_enqueue_wake_event(result);

        if (esp_timer_get_time() >= next_summary_us) {
            afe_pipeline_log_summary();
            next_summary_us = esp_timer_get_time() + AFE_PIPELINE_SUMMARY_PERIOD_US;
        }
    }
}

static void afe_pipeline_detection_task(void *argument)
{
    (void)argument;

    for (;;) {
        afe_pipeline_wake_event_t event = {0};
        if (xQueueReceive(s_runtime.wake_event_queue, &event, portMAX_DELAY) != pdPASS) {
            continue;
        }

        const uint32_t queued_now =
            (uint32_t)uxQueueMessagesWaiting(s_runtime.wake_event_queue);
        portENTER_CRITICAL(&s_metrics_lock);
        ++s_runtime.status.wake_events_processed;
        s_runtime.status.wake_event_queue_current = queued_now;
        portEXIT_CRITICAL(&s_metrics_lock);

        APP_LOGI(TAG, AFE_PIPELINE_WAKE_DETECTED_6CD23A10,
                 "WakeNet detected model=%d word=%d event=%llu fetch=%llu at_us=%lld",
                 (int)event.wakenet_model_index,
                 (int)event.wake_word_index,
                 (unsigned long long)event.event_sequence,
                 (unsigned long long)event.fetch_sequence,
                 (long long)event.detected_at_us);
    }
}

static void afe_pipeline_cleanup_start_failure(void)
{
    if (s_runtime.detection_task != NULL) {
        vTaskDeleteWithCaps(s_runtime.detection_task);
        s_runtime.detection_task = NULL;
    }
    if (s_runtime.feed_task != NULL) {
        vTaskDeleteWithCaps(s_runtime.feed_task);
        s_runtime.feed_task = NULL;
    }
    if (s_runtime.fetch_task != NULL) {
        vTaskDeleteWithCaps(s_runtime.fetch_task);
        s_runtime.fetch_task = NULL;
    }
    if (s_runtime.pcm_queue != NULL) {
        vQueueDelete(s_runtime.pcm_queue);
        s_runtime.pcm_queue = NULL;
    }
    if (s_runtime.wake_event_queue != NULL) {
        vQueueDelete(s_runtime.wake_event_queue);
        s_runtime.wake_event_queue = NULL;
    }
    if ((s_runtime.iface != NULL) && (s_runtime.afe != NULL)) {
        s_runtime.iface->destroy(s_runtime.afe);
        s_runtime.afe = NULL;
    }
    s_runtime.iface = NULL;
    if (s_runtime.models != NULL) {
        esp_srmodel_deinit(s_runtime.models);
        s_runtime.models = NULL;
    }
}

esp_err_t afe_pipeline_start(void)
{
    if (s_runtime.started) {
        return ESP_ERR_INVALID_STATE;
    }

    afe_pipeline_status_t initial = {0};
    initial.task_count_baseline = (uint32_t)uxTaskGetNumberOfTasks();
    afe_pipeline_take_heap_snapshot(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, &initial.baseline_internal);
    afe_pipeline_take_heap_snapshot(
        MALLOC_CAP_DMA | MALLOC_CAP_8BIT, &initial.baseline_dma);
    afe_pipeline_take_heap_snapshot(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, &initial.baseline_psram);
    afe_pipeline_take_cpu_snapshot(&initial.baseline_cpu);
    initial.last_error = ESP_OK;

    s_runtime.models = esp_srmodel_init("model");
    if (s_runtime.models == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (esp_srmodel_exists(s_runtime.models, s_wakenet_model_name) < 0) {
        afe_pipeline_cleanup_start_failure();
        return ESP_ERR_NOT_FOUND;
    }

    afe_config_t *config = afe_config_init(
        "M", s_runtime.models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (config == NULL) {
        afe_pipeline_cleanup_start_failure();
        return ESP_ERR_NO_MEM;
    }

    /* Keep the accepted transport intact; enable only the requested algorithms. */
    config->aec_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->vad_init = true;
    config->vad_model_name = NULL;
    config->vad_mode = VAD_MODE_0;
    config->agc_init = false;
    config->wakenet_init = true;
    config->wakenet_model_name = s_wakenet_model_name;
    config->wakenet_model_name_2 = NULL;
    config->wakenet_mode = DET_MODE_90;

    s_runtime.iface = esp_afe_handle_from_config(config);
    s_runtime.afe = (s_runtime.iface == NULL) ? NULL :
        s_runtime.iface->create_from_config(config);
    afe_config_free(config);
    if (s_runtime.afe == NULL) {
        afe_pipeline_cleanup_start_failure();
        return ESP_FAIL;
    }

    s_runtime.feed_samples_per_channel =
        s_runtime.iface->get_feed_chunksize(s_runtime.afe);
    s_runtime.fetch_samples = s_runtime.iface->get_fetch_chunksize(s_runtime.afe);
    s_runtime.feed_channels = s_runtime.iface->get_feed_channel_num(s_runtime.afe);
    s_runtime.fetch_channels = s_runtime.iface->get_fetch_channel_num(s_runtime.afe);
    const size_t feed_samples_total =
        ((s_runtime.feed_samples_per_channel > 0) && (s_runtime.feed_channels > 0)) ?
        ((size_t)s_runtime.feed_samples_per_channel * (size_t)s_runtime.feed_channels) : 0U;
    if ((s_runtime.iface->get_samp_rate(s_runtime.afe) !=
         (int)AUDIO_MANAGER_STREAM_SAMPLE_RATE_HZ) ||
        (s_runtime.feed_channels != (int)AUDIO_MANAGER_STREAM_CHANNELS) ||
        (s_runtime.fetch_channels != 1) ||
        (s_runtime.fetch_samples <= 0) ||
        (feed_samples_total == 0U) ||
        (feed_samples_total > AFE_PIPELINE_MAX_FEED_SAMPLES)) {
        afe_pipeline_cleanup_start_failure();
        return ESP_ERR_INVALID_SIZE;
    }

    initial.feed_samples_per_channel = s_runtime.feed_samples_per_channel;
    initial.fetch_samples = s_runtime.fetch_samples;
    initial.feed_channels = s_runtime.feed_channels;
    initial.fetch_channels = s_runtime.fetch_channels;
    s_runtime.status = initial;
    s_runtime.pcm_queue = xQueueCreateWithCaps(
        AFE_PIPELINE_PCM_QUEUE_LENGTH, sizeof(afe_pipeline_pcm_frame_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_runtime.wake_event_queue = xQueueCreateWithCaps(
        AFE_PIPELINE_WAKE_EVENT_QUEUE_LENGTH, sizeof(afe_pipeline_wake_event_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if ((s_runtime.pcm_queue == NULL) || (s_runtime.wake_event_queue == NULL)) {
        afe_pipeline_cleanup_start_failure();
        return ESP_ERR_NO_MEM;
    }

    s_runtime.started = true;
    if (xTaskCreateWithCaps(
            afe_pipeline_fetch_task, "afe_fetch", AFE_PIPELINE_TASK_STACK_BYTES,
            NULL, AFE_PIPELINE_TASK_PRIORITY, &s_runtime.fetch_task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreateWithCaps(
            afe_pipeline_feed_task, "afe_feed", AFE_PIPELINE_TASK_STACK_BYTES,
            NULL, AFE_PIPELINE_TASK_PRIORITY, &s_runtime.feed_task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreateWithCaps(
            afe_pipeline_detection_task, "afe_detect",
            AFE_PIPELINE_DETECTION_TASK_STACK_BYTES, NULL,
            AFE_PIPELINE_DETECTION_TASK_PRIORITY, &s_runtime.detection_task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_runtime.started = false;
        afe_pipeline_cleanup_start_failure();
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t registration_result =
        audio_manager_stream_register_local_monitor_callback(
            afe_pipeline_pcm_callback, NULL);
    if (registration_result != ESP_OK) {
        s_runtime.started = false;
        afe_pipeline_cleanup_start_failure();
        return registration_result;
    }

    const esp_err_t monitor_result = audio_manager_start_local_monitor();
    if (monitor_result != ESP_OK) {
        (void)audio_manager_stream_register_local_monitor_callback(NULL, NULL);
        s_runtime.started = false;
        afe_pipeline_cleanup_start_failure();
        return monitor_result;
    }

    portENTER_CRITICAL(&s_metrics_lock);
    s_runtime.status.started = true;
    portEXIT_CRITICAL(&s_metrics_lock);
    APP_LOGI(TAG, AFE_PIPELINE_STARTED_9D0CE2A7,
             "AFE proof started feed=%d x %d fetch=%d; feed/fetch workers p%u internal stack=%u",
             s_runtime.feed_samples_per_channel, s_runtime.feed_channels,
             s_runtime.fetch_samples, (unsigned int)AFE_PIPELINE_TASK_PRIORITY,
             (unsigned int)AFE_PIPELINE_TASK_STACK_BYTES);
    APP_LOGI(TAG, AFE_PIPELINE_WAKE_VAD_STARTED_A59D8BDF,
             "AFE WakeNet+VAD started model=%s detect_queue=%u worker p%u stack=%u; AEC/SE/NS/AGC disabled",
             s_wakenet_model_name, (unsigned int)AFE_PIPELINE_WAKE_EVENT_QUEUE_LENGTH,
             (unsigned int)AFE_PIPELINE_DETECTION_TASK_PRIORITY,
             (unsigned int)AFE_PIPELINE_DETECTION_TASK_STACK_BYTES);
    APP_LOGI(TAG, AFE_PIPELINE_BASELINE_2DA8F530,
             "AFE baseline before create task=%u internal=%u/%u/%u dma=%u/%u/%u "
             "psram=%u/%u/%u cpu=%s%u.%u%%",
             (unsigned int)initial.task_count_baseline,
             (unsigned int)initial.baseline_internal.free_bytes,
             (unsigned int)initial.baseline_internal.minimum_free_bytes,
             (unsigned int)initial.baseline_internal.largest_free_block_bytes,
             (unsigned int)initial.baseline_dma.free_bytes,
             (unsigned int)initial.baseline_dma.minimum_free_bytes,
             (unsigned int)initial.baseline_dma.largest_free_block_bytes,
             (unsigned int)initial.baseline_psram.free_bytes,
             (unsigned int)initial.baseline_psram.minimum_free_bytes,
             (unsigned int)initial.baseline_psram.largest_free_block_bytes,
             initial.baseline_cpu.sample_valid ? "" : "unavailable/",
             (unsigned int)(initial.baseline_cpu.cpu_used_x10 / 10U),
             (unsigned int)(initial.baseline_cpu.cpu_used_x10 % 10U));
    return ESP_OK;
}

esp_err_t afe_pipeline_get_status(afe_pipeline_status_t *status)
{
    QueueHandle_t pcm_queue = NULL;
    QueueHandle_t wake_event_queue = NULL;
    TaskHandle_t feed_task = NULL;
    TaskHandle_t fetch_task = NULL;
    TaskHandle_t detection_task = NULL;

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_metrics_lock);
    *status = s_runtime.status;
    pcm_queue = s_runtime.pcm_queue;
    wake_event_queue = s_runtime.wake_event_queue;
    feed_task = s_runtime.feed_task;
    fetch_task = s_runtime.fetch_task;
    detection_task = s_runtime.detection_task;
    portEXIT_CRITICAL(&s_metrics_lock);

    if (pcm_queue != NULL) {
        status->pcm_queue_current =
            (uint32_t)uxQueueMessagesWaiting(pcm_queue);
    }
    if (wake_event_queue != NULL) {
        status->wake_event_queue_current =
            (uint32_t)uxQueueMessagesWaiting(wake_event_queue);
    }
    if (feed_task != NULL) {
        status->feed_task_stack_hwm_words =
            (uint32_t)uxTaskGetStackHighWaterMark(feed_task);
    }
    if (fetch_task != NULL) {
        status->fetch_task_stack_hwm_words =
            (uint32_t)uxTaskGetStackHighWaterMark(fetch_task);
    }
    if (detection_task != NULL) {
        status->detection_task_stack_hwm_words =
            (uint32_t)uxTaskGetStackHighWaterMark(detection_task);
    }

    afe_pipeline_take_current_resources(status);
    return ESP_OK;
}

#else

esp_err_t afe_pipeline_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t afe_pipeline_get_status(afe_pipeline_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->last_error = ESP_ERR_NOT_SUPPORTED;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
