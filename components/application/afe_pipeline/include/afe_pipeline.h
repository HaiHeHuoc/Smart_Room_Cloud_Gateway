/**
 * @file afe_pipeline.h
 * @brief Bounded Phase-24.2 ESP-SR producer/consumer and detection proof.
 *
 * This component never owns I2S, DMA, GPIO, PTT, UI, or Xiaozhi. When enabled
 * at build time, it accepts copied PCM16 frames from audio_manager's separate
 * local-monitor observer, feeds ESP-SR, continuously drains fetch(), and
 * copies WakeNet/VAD observations into project-owned bounded state.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint32_t free_bytes;
    uint32_t minimum_free_bytes;
    uint32_t largest_free_block_bytes;
} afe_pipeline_heap_snapshot_t;

typedef struct
{
    bool sample_valid;
    uint32_t cpu_used_x10;
    uint32_t cpu_peak_500ms_x10;
    uint32_t cpu_idle_x10;
    int64_t captured_at_us;
} afe_pipeline_cpu_snapshot_t;

/**
 * @brief A copied WakeNet observation with no ESP-SR-owned lifetime.
 *
 * The fetch worker creates this fixed-size event only after a WakeNet result.
 * It never exposes an @c afe_fetch_result_t pointer across the component
 * boundary. Phase 24.2.2's internal consumer only records the observation;
 * it does not start Xiaozhi, request PTT, alter audio ownership, or touch UI.
 */
typedef struct
{
    uint64_t event_sequence;
    uint64_t fetch_sequence;
    int64_t detected_at_us;
    int32_t wake_word_index;
    int32_t wakenet_model_index;
} afe_pipeline_wake_event_t;

typedef struct
{
    bool started;
    int32_t feed_samples_per_channel;
    int32_t fetch_samples;
    int32_t feed_channels;
    int32_t fetch_channels;

    uint64_t pcm_frames_received;
    uint64_t feed_attempts;
    uint64_t feed_accepted;
    uint64_t feed_rejected;
    uint64_t feed_errors;
    uint64_t fetch_calls;
    uint64_t fetch_success;
    uint64_t fetch_timeout;
    uint64_t fetch_errors;
    uint32_t max_feed_us;
    uint32_t max_fetch_us;

    uint64_t wake_detections;
    uint64_t wake_events_enqueued;
    uint64_t wake_events_dropped;
    uint64_t wake_events_processed;
    int64_t last_wake_timestamp_us;
    int32_t last_wake_word_index;
    int32_t last_wakenet_model_index;

    uint64_t vad_speech_results;
    uint64_t vad_non_speech_results;
    uint64_t vad_transitions;
    bool vad_state_valid;
    bool vad_speech;

    uint32_t pcm_queue_current;
    uint32_t pcm_queue_peak;
    uint64_t pcm_queue_drops;
    uint32_t wake_event_queue_current;
    uint32_t wake_event_queue_peak;
    uint32_t feed_task_stack_hwm_words;
    uint32_t fetch_task_stack_hwm_words;
    uint32_t detection_task_stack_hwm_words;
    uint32_t task_count_baseline;
    uint32_t task_count_current;

    afe_pipeline_heap_snapshot_t baseline_internal;
    afe_pipeline_heap_snapshot_t baseline_dma;
    afe_pipeline_heap_snapshot_t baseline_psram;
    afe_pipeline_heap_snapshot_t current_internal;
    afe_pipeline_heap_snapshot_t current_dma;
    afe_pipeline_heap_snapshot_t current_psram;
    afe_pipeline_cpu_snapshot_t baseline_cpu;
    afe_pipeline_cpu_snapshot_t current_cpu;
    esp_err_t last_error;
} afe_pipeline_status_t;

/**
 * @brief Start the default-off continuous AFE soak proof once audio_manager is
 *        ready.
 *
 * Task context only. The component creates a bounded PCM queue, a feed worker,
 * and a fetch worker, then requests audio_manager's dedicated local-monitor
 * mode. Phase 24.2.2 enables only the packed @c wn9_hiesp WakeNet model and
 * observational WebRTC VAD. Wake observations are copied to a bounded queue;
 * a full queue is counted and dropped without delaying fetch(). No runtime
 * reset/rearm, PTT, playback, UI, network, or Xiaozhi lifecycle is implemented.
 */
esp_err_t afe_pipeline_start(void);

/**
 * @brief Copy bounded diagnostics and resource snapshots without measuring or
 *        changing the AFE pipeline.
 */
esp_err_t afe_pipeline_get_status(afe_pipeline_status_t *status);

#ifdef __cplusplus
}
#endif
