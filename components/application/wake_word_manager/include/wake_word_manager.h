#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t kind;
    uint32_t state;
    uint32_t generation;
    uint32_t duration_ms;
    uint64_t frame_sequence;
    uint64_t timestamp_us;
    uint32_t wake_word_index;
    uint32_t detection_count;
} wake_word_manager_event_t;

typedef enum {
    WAKE_WORD_STATE_DISABLED = 0,
    WAKE_WORD_STATE_ARMED,
    WAKE_WORD_STATE_WAIT_FOR_SPEECH,
    WAKE_WORD_STATE_LISTENING,
    WAKE_WORD_STATE_UTTERANCE_COMPLETE,
    WAKE_WORD_STATE_REARM_GUARD,
    WAKE_WORD_STATE_SUPPRESSED,
    WAKE_WORD_STATE_ERROR,
} wake_word_manager_state_t;

typedef enum {
    WAKE_WORD_EVENT_WAKE_DETECTED = 0,
    WAKE_WORD_EVENT_SPEECH_STARTED,
    WAKE_WORD_EVENT_UTTERANCE_COMPLETE,
    WAKE_WORD_EVENT_NO_SPEECH_TIMEOUT,
    WAKE_WORD_EVENT_MAX_UTTERANCE,
    WAKE_WORD_EVENT_CANCELLED,
    WAKE_WORD_EVENT_REARMED,
} wake_word_manager_event_kind_t;

typedef void (*wake_word_manager_event_callback_t)(
    const wake_word_manager_event_t *event,
    void *user_context);

typedef struct {
    bool initialized;
    bool running;
    bool enabled;
    bool muted;
    bool detector_ready;
    bool capture_requested;
    /** True only while a WakeNet-triggered voice turn needs local VAD input. */
    bool handsfree_handoff_active;
    /** True after at least one complete AFE input chunk was accepted. */
    bool afe_input_ready;
    uint32_t detections;
    wake_word_manager_state_t state;
    uint32_t wake_generation;
    uint32_t speech_start_count;
    uint32_t utterance_complete_count;
    uint32_t no_speech_timeout_count;
    uint32_t max_utterance_timeout_count;
    uint32_t cancel_count;
    uint32_t rearm_count;
    uint32_t pcm_dropped_disabled;
    uint32_t pcm_suppressed_playback;
    uint32_t pcm_suppressed_recording;
    uint32_t pcm_sequence_gaps;
    uint32_t debounce_suppressed;
    uint32_t last_wake_word_index;
    uint32_t mute_cancel_count;
    uint32_t playback_suppression_count;
    uint32_t recording_suppression_count;
    uint32_t max_total_turn_timeout_count;
    uint32_t afe_feed_samples_per_channel;
    uint32_t afe_fetch_samples;
    uint8_t afe_feed_channels;
    uint8_t afe_fetch_channels;
    uint32_t wake_pcm_queue_peak_depth;
    uint64_t wake_pcm_queue_drops;
    uint64_t pcm_frames_received;
    uint64_t afe_feed_calls;
    uint64_t afe_fetch_calls;
    uint64_t afe_feed_failures;
    uint64_t afe_fetch_failures;
    uint64_t afe_feed_dropped_reset;
    uint32_t max_afe_feed_us;
    uint32_t max_afe_fetch_us;
    uint32_t feed_task_stack_high_water_bytes;
    uint32_t fetch_task_stack_high_water_bytes;
    uint32_t internal_largest_before_afe_bytes;
    uint32_t internal_largest_after_afe_bytes;
    uint64_t last_transition_us;
    uint64_t last_wake_us;
    uint64_t last_speech_start_us;
    uint32_t last_utterance_duration_ms;
    esp_err_t last_error;
} wake_word_manager_status_t;

/** Initialize the private ESP-SR model/AFE lifecycle. Task context only. */
esp_err_t wake_word_manager_init(void);
/** Start the private local AFE feed/fetch workers. This does not enable detection. */
esp_err_t wake_word_manager_start(void);
/** Explicitly enable or disable local WakeNet capture and processing. */
esp_err_t wake_word_manager_set_enabled(bool enabled);
/** Mute is local-only: it cancels any current local wake turn and sends no PCM. */
esp_err_t wake_word_manager_set_muted(bool muted);

/**
 * @brief Retain local VAD input for one accepted WakeNet-to-voice handoff.
 *
 * This does not start Xiaozhi, I2S, or a second microphone reader. The
 * application composition root sets it before queuing its virtual voice-turn
 * trigger, and clears it when that turn ends or cannot be admitted.
 */
esp_err_t wake_word_manager_set_handsfree_handoff_active(bool active);
esp_err_t wake_word_manager_get_status(wake_word_manager_status_t *status);
esp_err_t wake_word_manager_register_callback(
    wake_word_manager_event_callback_t callback,
    void *user_context);

#ifdef __cplusplus
}
#endif
