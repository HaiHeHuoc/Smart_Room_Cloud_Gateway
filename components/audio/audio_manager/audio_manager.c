/**
 * @file audio_manager.c
 * @brief Production-controlled INMP441/WAV -> MAX98357A audio manager.
 *
 * The proven NewSolution record/DSP/playback path remains a default-off golden
 * regression mode. Normal startup reaches IDLE and the single manager task
 * owns bounded commands, sources, cancellation cleanup, and I2S lifecycle so
 * the same status model can feed app_gui without coupling audio_manager to
 * LVGL.
 */

/* Includes ----------------------------------------------------------------- */
#include "audio_manager.h"
#include "audio_dsp.h"
#include "audio_manager_pcm_stream.h"
#include "audio_manager_pcm_stream_core.h"
#include "audio_manager_playback_control_policy.h"
#include "audio_manager_stream_internal.h"
#include "audio_wav.h"
#include "audio_wav_prefetch.h"
#include "sd_card_manager.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app_log.h"
#include "voice_recording_critical.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Macros ------------------------------------------------------------------- */
#define AUDIO_MANAGER_DEFAULT_VOLUME_PERCENT          100U

#define AUDIO_MANAGER_TASK_NAME                       "audio_manager"
#define AUDIO_MANAGER_TASK_STACK_SIZE                 8192U
#define AUDIO_MANAGER_INTER_CYCLE_DELAY_MS            250U
#define AUDIO_MANAGER_MUTEX_TIMEOUT_MS                100U
#define AUDIO_MANAGER_COMMAND_QUEUE_LENGTH               2U
#define AUDIO_MANAGER_COMMAND_POLL_MS                   100U
#define AUDIO_MANAGER_TASK_START_TIMEOUT_MS            2000U
#define AUDIO_MANAGER_TASK_STOP_TIMEOUT_MS             5000U
#define AUDIO_MANAGER_WAV_PREFETCH_WAIT_POLL_MS          100U
#define AUDIO_MANAGER_WAV_PREFETCH_READER_PRIORITY         5U
#define AUDIO_MANAGER_PCM_STREAM_RING_SAMPLES \
    (128U * AUDIO_MANAGER_PCM_STREAM_MAX_WRITE_SAMPLES)
/* 0.96 s is enough to absorb normal packet scheduling jitter while allowing
 * short Xiaozhi replies to start before a delayed TTS_STOP arrives. The
 * larger 7.68 s ingress ring and the post-start starvation recovery remain
 * responsible for longer network gaps. The bounded prefill deadline begins
 * only after the first PCM packet; voice_downlink owns the separate server
 * response-start watchdog. */
#define AUDIO_MANAGER_PCM_STREAM_PREFILL_SAMPLES \
    (16U * AUDIO_MANAGER_PCM_STREAM_MAX_WRITE_SAMPLES)
#define AUDIO_MANAGER_PCM_STREAM_PREFILL_WAIT_MS        5000U
#define AUDIO_MANAGER_PCM_STREAM_INITIAL_WAIT_POLL_MS     20U
#define AUDIO_MANAGER_PCM_STREAM_STARVATION_WAIT_MS     8000U
#define AUDIO_MANAGER_PCM_STREAM_SILENCE_BLOCK_MS \
    ((AUDIO_MANAGER_FRAMES_PER_BLOCK * 1000U) / AUDIO_MANAGER_SAMPLE_RATE_HZ)
#define AUDIO_MANAGER_PCM_STREAM_PRE_PLAYBACK_SILENCE_BLOCKS 4U
#define AUDIO_MANAGER_WAV_PREFETCH_BYTES_PER_SECOND \
    (AUDIO_MANAGER_SAMPLE_RATE_HZ * sizeof(int16_t))
#define AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES \
    (AUDIO_MANAGER_WAV_PREFETCH_BYTES_PER_SECOND * \
     CONFIG_AUDIO_MANAGER_WAV_PREFETCH_SECONDS)
#define AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS \
    CONFIG_AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS

#define AUDIO_MANAGER_TASK_READY_BIT  ((EventBits_t)(1U << 0U))
#define AUDIO_MANAGER_TASK_STOPPED_BIT ((EventBits_t)(1U << 1U))

/* Production defaults are fixed; test-only Kconfig overrides were retired. */
#define AUDIO_MANAGER_DEFAULT_RECORD_SECONDS          5U
#define AUDIO_MANAGER_TASK_PRIORITY                   7U

/* Six 16-ms RX descriptors retain 96 ms of microphone DMA slack while freeing
 * 4 KiB of Internal/DMA heap for the concurrent TLS uplink record. */
#define AUDIO_MANAGER_SAMPLE_RATE_HZ                  AUDIO_DSP_SAMPLE_RATE_HZ
#define AUDIO_MANAGER_SLOT_COUNT                      2U
#define AUDIO_MANAGER_FRAMES_PER_BLOCK                256U
#define AUDIO_MANAGER_DMA_DESC_NUM                    6U
#define AUDIO_MANAGER_I2S_TIMEOUT_MS                  1000U

/* Proven INMP441/MAX98357 cycle policy. */
#define AUDIO_MANAGER_STARTUP_DISCARD_BLOCKS          40U
#define AUDIO_MANAGER_SLOT_DETECT_BLOCKS              20U
#define AUDIO_MANAGER_PRE_PLAYBACK_DELAY_MS           500U
#define AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS     32U
#define AUDIO_MANAGER_POST_PLAYBACK_SILENCE_BLOCKS    32U

_Static_assert(AUDIO_MANAGER_SLOT_COUNT == 2U,
               "Standard I2S transport requires two slots");
_Static_assert(AUDIO_MANAGER_SAMPLE_RATE_HZ == AUDIO_SAMPLE_RATE_HZ,
               "DSP and board sample rates must match");
_Static_assert(AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS > 0U,
               "Manual recording duration must be positive");
_Static_assert((AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES % sizeof(int16_t)) == 0U,
               "WAV prefetch slots must contain complete PCM16 samples");
_Static_assert(AUDIO_MANAGER_PCM_STREAM_PREFILL_SAMPLES <=
                   AUDIO_MANAGER_PCM_STREAM_RING_SAMPLES,
               "PCM stream prefill must fit its bounded ring");
_Static_assert(AUDIO_MANAGER_PCM_STREAM_SILENCE_BLOCK_MS > 0U,
               "PCM stream silence block duration must be nonzero");

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "AUDIO_MANAGER";

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    MICROPHONE_SLOT_LEFT = 0,
    MICROPHONE_SLOT_RIGHT = 1,
} microphone_slot_t;

typedef enum
{
    AUDIO_RECORD_CONTROL_GOLDEN_FIXED = 0,
    AUDIO_RECORD_CONTROL_PRODUCTION_FIXED,
    AUDIO_RECORD_CONTROL_MANUAL,
} audio_record_control_t;

typedef enum
{
    AUDIO_RECORD_STOP_NONE = 0,
    AUDIO_RECORD_STOP_MANUAL,
    AUDIO_RECORD_STOP_ABORT,
} audio_record_stop_reason_t;

typedef struct
{
    uint64_t absolute_sum;
    uint32_t peak;
    uint32_t sample_count;
} slot_stats_t;

typedef struct
{
    uint32_t total_yields;
} dsp_scheduler_stats_t;

typedef struct
{
    uint32_t fixed_scale_gain_q16;
    uint32_t output_peak_pcm16;
    uint64_t data_bytes_read;
    uint64_t data_bytes_streamed;
    uint32_t expected_data_bytes;
    uint32_t expected_duration_ms;
    uint32_t read_count;
    uint32_t read_failure_count;
    uint32_t max_wav_read_duration_us;
    uint32_t prefetch_block_bytes;
    uint32_t prefetch_blocks_filled;
    uint32_t prefetch_fill_failure_count;
    uint32_t max_prefetch_fill_duration_us;
    uint64_t prefetch_recovery_data_offset;
    uint32_t prefetch_recovery_attempt_count;
    uint32_t prefetch_recovery_success_count;
    uint32_t prefetch_recovery_wait_ms;
    uint32_t initial_prefetch_wait_ms;
    uint32_t prefetch_wait_ms;
    uint32_t prefetch_starvation_count;
    uint32_t prefetch_task_stack_high_water;
    uint32_t playback_elapsed_ms;
} audio_wav_playback_metrics_t;

typedef struct
{
    size_t samples_recorded;
    int32_t dc_offset_pcm24;
    uint32_t raw_average_pcm24;
    uint32_t raw_peak_pcm24;
    uint32_t band_average_pcm24;
    uint32_t band_peak_pcm24;
    uint32_t ns_average_pcm24;
    uint32_t ns_peak_pcm24;
    uint32_t dsp_processing_ms;
    uint32_t dsp_realtime_factor_milli;
    uint32_t dsp_total_yields;
    audio_dsp_ns_metrics_t ns;
    audio_dsp_playback_stats_t playback;
    audio_wav_playback_metrics_t wav;
} audio_cycle_metrics_t;

typedef struct
{
    bool capture_i2s_active;
    bool playback_i2s_active;
    uint32_t rx_overflow_count;
    uint32_t rx_timeout_count;
    uint32_t tx_queue_overflow_count;
    uint32_t tx_timeout_count;
    uint32_t tx_partial_write_count;
    uint32_t max_rx_read_duration_us;
    uint32_t max_tx_write_duration_us;
    uint64_t rx_bytes_requested;
    uint64_t rx_bytes_read;
    uint64_t tx_bytes_requested;
    uint64_t tx_bytes_written;
} audio_manager_diagnostics_t;

/* Only audio_manager task or lifecycle code may select/release a source. */
typedef enum
{
    AUDIO_PLAYBACK_SOURCE_NONE = 0,
    AUDIO_PLAYBACK_SOURCE_RECORDED_PCM24,
    AUDIO_PLAYBACK_SOURCE_WAV_PCM16,
    AUDIO_PLAYBACK_SOURCE_PCM16_STREAM,
} audio_playback_source_kind_t;

typedef struct
{
    audio_playback_source_kind_t kind;
    size_t recorded_sample_count;
    uint32_t stream_generation;
    audio_wav_prefetch_t wav_prefetch;
} audio_playback_source_t;

typedef enum
{
    AUDIO_PLAYBACK_FLOW_CONTINUE = 0,
    AUDIO_PLAYBACK_FLOW_PAUSE,
    AUDIO_PLAYBACK_FLOW_RESTART,
    AUDIO_PLAYBACK_FLOW_CANCEL,
} audio_playback_flow_t;

/** Bounded owner-only identity retained while a local source is paused. */
typedef struct
{
    bool valid;
    audio_playback_source_kind_t kind;
    uint32_t generation;
    audio_manager_playback_pause_reason_t pause_reason;
    uint64_t position_frames;
    uint64_t total_frames;
    size_t recorded_sample_count;
    char wav_path[AUDIO_MANAGER_WAV_PATH_MAX_BYTES];
    audio_wav_info_t wav_info;
    bool wav_info_valid;
} audio_playback_resume_context_t;

typedef enum
{
    AUDIO_MANAGER_COMMAND_RECORD_FIXED = 0,
    AUDIO_MANAGER_COMMAND_RECORD_MANUAL,
    AUDIO_MANAGER_COMMAND_PLAY_RECORDED,
    AUDIO_MANAGER_COMMAND_PLAY_WAV,
    AUDIO_MANAGER_COMMAND_PLAY_PCM16_STREAM,
    AUDIO_MANAGER_COMMAND_SHUTDOWN,
} audio_manager_command_kind_t;

typedef struct
{
    audio_manager_command_kind_t kind;
    char wav_path[AUDIO_MANAGER_WAV_PATH_MAX_BYTES];
    uint32_t stream_generation;
    bool retained_resume;
    bool restart_from_beginning;
} audio_manager_command_t;

typedef enum
{
    AUDIO_MANAGER_OPERATION_NONE = 0,
    AUDIO_MANAGER_OPERATION_RECORD_FIXED,
    AUDIO_MANAGER_OPERATION_RECORD_MANUAL,
    AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK,
    AUDIO_MANAGER_OPERATION_WAV,
    AUDIO_MANAGER_OPERATION_PCM16_STREAM,
    AUDIO_MANAGER_OPERATION_STABILITY,
} audio_manager_operation_t;

typedef struct
{
    bool task_running;
    bool shutdown_requested;
    bool cancel_requested;
    bool record_stop_requested;
    bool pause_requested;
    bool resume_requested;
    bool restart_requested;
    audio_manager_playback_pause_reason_t requested_pause_reason;
    audio_manager_operation_t operation;
    uint32_t next_playback_generation;
    audio_manager_playback_status_t playback_status;
} audio_manager_control_t;

typedef struct
{
    bool initialized;
    bool rx_enabled;
    bool tx_enabled;

    audio_manager_config_t config;

    /* One manager-owned source slot; only this task/lifecycle owns it. */
    audio_playback_source_t playback_source;
    audio_playback_resume_context_t playback_resume;

    size_t sample_capacity;
    size_t fixed_record_sample_count;
    size_t manual_record_sample_limit;
    size_t recording_bytes;
    int32_t *recording_pcm24;
    audio_dsp_workspace_t *dsp_workspace;

    /* The downlink producer copies bounded PCM16 packets here. The manager
     * task is the only consumer and the only I2S/TX owner. */
    int16_t *pcm_stream_storage;
    audio_manager_pcm_stream_core_t pcm_stream;
    SemaphoreHandle_t pcm_stream_mutex;
    bool pcm_stream_tx_started;

    bool recorded_audio_valid;
    size_t recorded_sample_count;

    i2s_chan_handle_t rx_channel;
    i2s_chan_handle_t tx_channel;

    TaskHandle_t task_handle;
    SemaphoreHandle_t status_mutex;
    QueueHandle_t command_queue;
    EventGroupHandle_t lifecycle_events;
    esp_err_t task_exit_result;

    audio_manager_status_t status;
    audio_manager_status_callback_t status_callback;
    void *status_callback_context;

} audio_manager_runtime_t;

/* Static Variables --------------------------------------------------------- */
static audio_manager_runtime_t s_runtime = {0};

/* Small I2S staging stays in Internal/DMA RAM; long history stays in PSRAM. */
DMA_ATTR static int32_t s_rx_block[
    AUDIO_MANAGER_FRAMES_PER_BLOCK * AUDIO_MANAGER_SLOT_COUNT];
DMA_ATTR static int16_t s_tx_block[
    AUDIO_MANAGER_FRAMES_PER_BLOCK * AUDIO_MANAGER_SLOT_COUNT];
/* Mono ingress is copied out of the PSRAM ring before it is mapped into the
 * DMA-capable stereo TX block. */
DMA_ATTR static int16_t s_pcm_stream_mono_block[AUDIO_MANAGER_FRAMES_PER_BLOCK];
DMA_ATTR static const int16_t s_silence_block[
    AUDIO_MANAGER_FRAMES_PER_BLOCK * AUDIO_MANAGER_SLOT_COUNT] = {0};

/* Both ISR callbacks and tasks update these diagnostics. */
DRAM_ATTR static portMUX_TYPE s_diagnostics_lock =
    portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR static audio_manager_diagnostics_t s_diagnostics = {0};

/* Short task/public API control handoff; no blocking work occurs under it. */
DRAM_ATTR static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR static audio_manager_control_t s_control = {0};

/* Function Prototypes ------------------------------------------------------ */
static bool audio_manager_rx_overflow_callback(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_context);
static bool audio_manager_tx_overflow_callback(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_context);

static bool audio_manager_take_status_mutex(const char *operation);
static bool audio_manager_take_pcm_stream_mutex(const char *operation);
static void audio_manager_reset_diagnostics(void);
static void audio_manager_snapshot_diagnostics(
    audio_manager_diagnostics_t *diagnostics);
static void audio_manager_set_capture_i2s_active(bool active);
static void audio_manager_set_playback_i2s_active(bool active);
static void audio_manager_reset_control(void);
static bool audio_manager_cancel_is_requested(void);
static audio_playback_flow_t audio_manager_playback_flow_requested(void);
static void audio_manager_begin_playback_control_locked(
    audio_manager_playback_source_t source,
    bool resumable,
    uint64_t total_frames);
static void audio_manager_abort_playback_control_begin(void);
static void audio_manager_set_playback_applied(void);
static void audio_manager_update_playback_position(uint64_t position_frames);
static void audio_manager_update_playback_total(uint64_t total_frames);
static void audio_manager_set_playback_paused(
    audio_manager_playback_pause_reason_t reason,
    uint64_t position_frames);
static void audio_manager_get_pause_metadata(
    audio_manager_playback_pause_reason_t *reason,
    uint32_t *generation);
static void audio_manager_complete_playback_control(esp_err_t result);
static void audio_manager_clear_resume_context(void);
static void audio_manager_restore_retained_playback_status(void);
static void audio_manager_suspend_operation(void);
static bool audio_manager_consume_resume_requested(void);
static esp_err_t audio_manager_queue_retained_playback(
    uint32_t expected_generation,
    bool restart_from_beginning);
static bool audio_manager_shutdown_is_requested(void);
static audio_record_stop_reason_t audio_manager_record_stop_reason(
    audio_record_control_t control);
static bool audio_manager_recorded_playback_cancel_enabled(void);
static void audio_manager_finish_operation(void);
static void audio_manager_finish_wav_operation(void);
static bool audio_manager_try_begin_stability_operation(void);
static void audio_manager_record_rx_io(
    esp_err_t result,
    size_t bytes_read,
    int64_t duration_us);
static void audio_manager_record_tx_io(
    esp_err_t result,
    size_t bytes_requested,
    size_t bytes_written,
    int64_t duration_us);
static void audio_manager_refresh_diagnostics_locked(void);
static void audio_manager_notify_status_changed(void);
static void audio_manager_set_state(audio_manager_state_t state);

static void log_heap_state(const char *label);
static esp_err_t hold_amplifier_data_low(void);

static esp_err_t start_i2s_rx(void);
static esp_err_t stop_i2s_rx(void);
static esp_err_t read_rx_block(size_t *frames_read);
static esp_err_t discard_microphone_startup(
    audio_record_control_t control,
    audio_record_stop_reason_t *stop_reason);
static void update_slot_stats(slot_stats_t *stats, int32_t sample);
static esp_err_t detect_microphone_slot(
    microphone_slot_t *selected_slot,
    audio_record_control_t control,
    audio_record_stop_reason_t *stop_reason);
static esp_err_t record_audio(
    microphone_slot_t selected_slot,
    size_t target_sample_count,
    audio_record_control_t control,
    size_t *samples_recorded,
    audio_record_stop_reason_t *stop_reason);

static esp_err_t start_i2s_tx(void);
static esp_err_t stop_i2s_tx(void);
static esp_err_t write_tx_frames(size_t frame_count);
static esp_err_t write_silence_blocks(
    uint32_t block_count,
    bool *cancelled);
static esp_err_t write_controlled_silence_blocks(
    uint32_t block_count,
    audio_playback_flow_t *flow);
static int32_t apply_playback_volume_percent(int32_t sample_pcm24);
static int16_t decode_wav_pcm16_le(const uint8_t *sample_bytes);
static int16_t apply_wav_volume_percent(int16_t sample_pcm16);
static esp_err_t play_recording(
    size_t start_sample_index,
    size_t sample_count,
    audio_dsp_playback_stats_t *stats,
    audio_playback_flow_t *flow,
    size_t *next_sample_index);
static esp_err_t play_wav_stream(
    audio_wav_prefetch_t *prefetch,
    uint64_t start_data_offset,
    audio_wav_playback_metrics_t *metrics,
    audio_playback_flow_t *flow,
    uint64_t *next_data_offset,
    audio_wav_info_t *info,
    bool *info_valid);
static esp_err_t play_pcm16_stream(
    uint32_t generation,
    bool *cancelled);
static esp_err_t audio_manager_copy_prefetch_metrics(
    audio_wav_prefetch_t *prefetch,
    audio_wav_playback_metrics_t *metrics);

static void dsp_cooperative_yield(void *context);
static void log_ns_metrics(const audio_dsp_ns_metrics_t *metrics);
static void log_playback_result(const audio_dsp_playback_stats_t *playback);

static esp_err_t record_once_controlled(
    size_t target_sample_count,
    audio_record_control_t control,
    size_t *samples_recorded,
    audio_record_stop_reason_t *stop_reason);
static esp_err_t record_once(size_t *samples_recorded);
static esp_err_t process_once(
    size_t sample_count,
    audio_cycle_metrics_t *metrics);
static esp_err_t playback_once(
    audio_playback_source_t *source,
    audio_cycle_metrics_t *metrics,
    bool *cancelled);
static esp_err_t audio_manager_select_recording_playback_source(
    size_t sample_count);
static esp_err_t audio_manager_select_wav_playback_source(
    const char *path,
    uint64_t committed_data_offset,
    const audio_wav_info_t *expected_info);
static esp_err_t audio_manager_select_pcm_stream_playback_source(
    uint32_t generation);
static esp_err_t audio_manager_release_playback_source(void);
static esp_err_t force_cycle_cleanup(void);
static esp_err_t run_cycle(audio_cycle_metrics_t *metrics);
static void audio_manager_handle_record_command(bool manual);
static void audio_manager_handle_recorded_playback_command(
    bool retained_resume,
    bool restart_from_beginning);
static void audio_manager_handle_wav_command(
    const char *path,
    bool retained_resume,
    bool restart_from_beginning);
static void audio_manager_handle_pcm_stream_command(uint32_t generation);
static void audio_manager_run_stability_iteration(void);
static bool audio_manager_stability_mode_enabled(void);
static bool audio_manager_mixed_stress_mode_enabled(void);
static esp_err_t audio_manager_queue_simple_operation(
    audio_manager_command_kind_t command_kind,
    audio_manager_operation_t operation,
    bool require_recorded_audio,
    const char *description);
static void log_cycle_diagnostics(
    uint32_t cycle,
    const audio_manager_diagnostics_t *before);
static void audio_manager_task(void *argument);

/* Static Functions: Status / Callback ------------------------------------- */
static bool audio_manager_take_status_mutex(const char *operation)
{
    if (s_runtime.status_mutex == NULL)
    {
        return false;
    }

    if (xSemaphoreTake(
            s_runtime.status_mutex,
            pdMS_TO_TICKS(AUDIO_MANAGER_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        return true;
    }

    APP_LOGE(
        TAG, STATUS_MUTEX_TIMEOUT_WHILE_S_18EB6A28,
        "Status mutex timeout while %s",
        (operation != NULL) ? operation : "updating diagnostics");
    return false;
}

static bool audio_manager_take_pcm_stream_mutex(const char *operation)
{
    if (s_runtime.pcm_stream_mutex == NULL)
    {
        return false;
    }

    if (xSemaphoreTake(
            s_runtime.pcm_stream_mutex,
            pdMS_TO_TICKS(AUDIO_MANAGER_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        return true;
    }

    APP_LOGE(
        TAG, PCM_STREAM_MUTEX_TIMEOUT_WHI_E592C47F,
        "PCM stream mutex timeout while %s",
        (operation != NULL) ? operation : "updating stream state");
    return false;
}

static void audio_manager_pcm_stream_notify_task(void)
{
    const TaskHandle_t task = s_runtime.task_handle;
    if (task != NULL)
    {
        xTaskNotifyGive(task);
    }
}

static void audio_manager_pcm_stream_close_from_owner(uint32_t generation)
{
    if (!audio_manager_take_pcm_stream_mutex("closing PCM stream"))
    {
        return;
    }

    if ((s_runtime.pcm_stream.generation == generation) &&
        s_runtime.pcm_stream.active)
    {
        (void)audio_manager_pcm_stream_core_close(
            &s_runtime.pcm_stream,
            generation);
    }
    s_runtime.pcm_stream_tx_started = false;
    xSemaphoreGive(s_runtime.pcm_stream_mutex);
}

esp_err_t audio_manager_pcm_stream_prepare(uint32_t generation)
{
    if ((generation == 0U) || !s_runtime.initialized ||
        (s_runtime.pcm_stream_mutex == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_pcm_stream_mutex("preparing PCM stream"))
    {
        return ESP_ERR_TIMEOUT;
    }

    if (s_runtime.pcm_stream.active)
    {
        xSemaphoreGive(s_runtime.pcm_stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = audio_manager_pcm_stream_core_prepare(
        &s_runtime.pcm_stream,
        generation);
    s_runtime.pcm_stream_tx_started = false;
    xSemaphoreGive(s_runtime.pcm_stream_mutex);
    return result;
}

esp_err_t audio_manager_pcm_stream_start(uint32_t generation)
{
    if ((generation == 0U) || !s_runtime.initialized ||
        (s_runtime.status_mutex == NULL) ||
        (s_runtime.command_queue == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (audio_manager_stability_mode_enabled())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_pcm_stream_mutex("starting PCM stream"))
    {
        return ESP_ERR_TIMEOUT;
    }
    const bool prepared = s_runtime.pcm_stream.active &&
                          !s_runtime.pcm_stream.aborted &&
                          (s_runtime.pcm_stream.generation == generation);
    xSemaphoreGive(s_runtime.pcm_stream_mutex);
    if (!prepared)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_status_mutex("queueing PCM stream playback"))
    {
        return ESP_ERR_TIMEOUT;
    }

    bool accepted = false;
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.task_running &&
        !s_control.shutdown_requested &&
        (s_control.operation == AUDIO_MANAGER_OPERATION_NONE) &&
        (s_runtime.status.state == AUDIO_MANAGER_STATE_IDLE))
    {
        s_control.operation = AUDIO_MANAGER_OPERATION_PCM16_STREAM;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
        audio_manager_begin_playback_control_locked(
            AUDIO_MANAGER_PLAYBACK_SOURCE_PCM16_STREAM,
            false,
            0U);
        accepted = true;
    }
    portEXIT_CRITICAL(&s_control_lock);

    if (!accepted)
    {
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const audio_manager_command_t command = {
        .kind = AUDIO_MANAGER_COMMAND_PLAY_PCM16_STREAM,
        .stream_generation = generation,
    };
    if (xQueueSend(s_runtime.command_queue, &command, 0U) != pdTRUE)
    {
        audio_manager_abort_playback_control_begin();
        audio_manager_finish_operation();
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_runtime.status_mutex);
    audio_manager_pcm_stream_notify_task();
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_write(
    uint32_t generation,
    const int16_t *samples,
    size_t sample_count)
{
    if ((generation == 0U) || (samples == NULL) || (sample_count == 0U) ||
        !s_runtime.initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_manager_take_pcm_stream_mutex("writing PCM stream"))
    {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t result = audio_manager_pcm_stream_core_write(
        &s_runtime.pcm_stream,
        generation,
        samples,
        sample_count);
    xSemaphoreGive(s_runtime.pcm_stream_mutex);
    if (result == ESP_OK)
    {
        audio_manager_pcm_stream_notify_task();
    }
    return result;
}

esp_err_t audio_manager_pcm_stream_finish(uint32_t generation)
{
    if ((generation == 0U) || !s_runtime.initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_manager_take_pcm_stream_mutex("finishing PCM stream"))
    {
        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t result = audio_manager_pcm_stream_core_finish(
        &s_runtime.pcm_stream,
        generation);
    xSemaphoreGive(s_runtime.pcm_stream_mutex);
    if (result == ESP_OK)
    {
        audio_manager_pcm_stream_notify_task();
    }
    return result;
}

esp_err_t audio_manager_pcm_stream_abort(uint32_t generation)
{
    if ((generation == 0U) || !s_runtime.initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_manager_take_pcm_stream_mutex("aborting PCM stream"))
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = audio_manager_pcm_stream_core_abort(
        &s_runtime.pcm_stream,
        generation);
    if (result == ESP_OK)
    {
        result = audio_manager_pcm_stream_core_close(
            &s_runtime.pcm_stream,
            generation);
    }
    s_runtime.pcm_stream_tx_started = false;
    xSemaphoreGive(s_runtime.pcm_stream_mutex);
    if (result == ESP_OK)
    {
        audio_manager_pcm_stream_notify_task();
    }
    return result;
}

esp_err_t audio_manager_pcm_stream_get_status(
    uint32_t generation,
    audio_manager_pcm_stream_status_t *status)
{
    if ((generation == 0U) || (status == NULL) || !s_runtime.initialized)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!audio_manager_take_pcm_stream_mutex("reading PCM stream status"))
    {
        return ESP_ERR_TIMEOUT;
    }

    audio_manager_pcm_stream_core_t snapshot = {0};
    const esp_err_t result = audio_manager_pcm_stream_core_get(
        &s_runtime.pcm_stream,
        generation,
        &snapshot);
    if (result == ESP_OK)
    {
        *status = (audio_manager_pcm_stream_status_t) {
            .active = snapshot.active,
            .ended = snapshot.ended,
            .aborted = snapshot.aborted,
            .tx_started = s_runtime.pcm_stream_tx_started,
            .generation = snapshot.generation,
            .queued_samples = snapshot.queued_samples,
            .accepted_samples = snapshot.accepted_samples,
            .played_samples = snapshot.consumed_samples,
            .high_water_samples = snapshot.high_water_samples,
            .full_count = snapshot.full_count,
            .starvation_count = snapshot.starvation_count,
        };
    }
    xSemaphoreGive(s_runtime.pcm_stream_mutex);
    return result;
}

static void audio_manager_reset_diagnostics(void)
{
    portENTER_CRITICAL(&s_diagnostics_lock);
    s_diagnostics = (audio_manager_diagnostics_t) {0};
    portEXIT_CRITICAL(&s_diagnostics_lock);
}

static void audio_manager_snapshot_diagnostics(
    audio_manager_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&s_diagnostics_lock);
    *diagnostics = s_diagnostics;
    portEXIT_CRITICAL(&s_diagnostics_lock);
}

static void audio_manager_set_capture_i2s_active(bool active)
{
    portENTER_CRITICAL(&s_diagnostics_lock);
    s_diagnostics.capture_i2s_active = active;
    portEXIT_CRITICAL(&s_diagnostics_lock);
}

static void audio_manager_set_playback_i2s_active(bool active)
{
    portENTER_CRITICAL(&s_diagnostics_lock);
    s_diagnostics.playback_i2s_active = active;
    portEXIT_CRITICAL(&s_diagnostics_lock);
}

static void audio_manager_reset_control(void)
{
    portENTER_CRITICAL(&s_control_lock);
    const uint32_t generation_counter = s_control.next_playback_generation;
    s_control = (audio_manager_control_t) {0};
    s_control.next_playback_generation = generation_counter;
    s_control.playback_status.state = AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE;
    s_control.playback_status.source = AUDIO_MANAGER_PLAYBACK_SOURCE_NONE;
    s_control.playback_status.position_granularity_frames =
        AUDIO_MANAGER_PLAYBACK_POSITION_GRANULARITY_FRAMES;
    s_control.playback_status.last_control_result = ESP_OK;
    portEXIT_CRITICAL(&s_control_lock);
}

static bool audio_manager_cancel_is_requested(void)
{
    bool cancel_requested;

    portENTER_CRITICAL(&s_control_lock);
    cancel_requested = s_control.cancel_requested;
    portEXIT_CRITICAL(&s_control_lock);

    return cancel_requested;
}

static audio_playback_flow_t audio_manager_playback_flow_requested(void)
{
    audio_playback_flow_t flow = AUDIO_PLAYBACK_FLOW_CONTINUE;

    portENTER_CRITICAL(&s_control_lock);
    if (s_control.shutdown_requested || s_control.cancel_requested)
    {
        flow = AUDIO_PLAYBACK_FLOW_CANCEL;
    }
    else if (s_control.restart_requested)
    {
        flow = AUDIO_PLAYBACK_FLOW_RESTART;
    }
    else if (s_control.pause_requested)
    {
        flow = AUDIO_PLAYBACK_FLOW_PAUSE;
    }
    portEXIT_CRITICAL(&s_control_lock);
    return flow;
}

static void audio_manager_begin_playback_control_locked(
    audio_manager_playback_source_t source,
    bool resumable,
    uint64_t total_frames)
{
    uint32_t generation = ++s_control.next_playback_generation;
    if (generation == 0U)
    {
        generation = ++s_control.next_playback_generation;
    }

    s_control.pause_requested = false;
    s_control.resume_requested = false;
    s_control.restart_requested = false;
    s_control.requested_pause_reason = AUDIO_MANAGER_PLAYBACK_PAUSE_NONE;
    s_control.playback_status = (audio_manager_playback_status_t) {
        .state = AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING,
        .source = source,
        .pause_reason = AUDIO_MANAGER_PLAYBACK_PAUSE_NONE,
        .last_action = AUDIO_MANAGER_PLAYBACK_ACTION_NONE,
        .resumable = resumable,
        .generation = generation,
        .position_frames = 0U,
        .total_frames = total_frames,
        .position_granularity_frames =
            AUDIO_MANAGER_PLAYBACK_POSITION_GRANULARITY_FRAMES,
        .last_control_result = ESP_OK,
    };
}

static void audio_manager_abort_playback_control_begin(void)
{
    portENTER_CRITICAL(&s_control_lock);
    const audio_manager_playback_action_t last_action =
        s_control.playback_status.last_action;
    const esp_err_t last_result =
        s_control.playback_status.last_control_result;
    const uint32_t generation_counter = s_control.next_playback_generation;
    s_control.playback_status = (audio_manager_playback_status_t) {
        .state = AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE,
        .source = AUDIO_MANAGER_PLAYBACK_SOURCE_NONE,
        .last_action = last_action,
        .position_granularity_frames =
            AUDIO_MANAGER_PLAYBACK_POSITION_GRANULARITY_FRAMES,
        .last_control_result = last_result,
    };
    s_control.next_playback_generation = generation_counter;
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_set_playback_applied(void)
{
    portENTER_CRITICAL(&s_control_lock);
    audio_manager_playback_control_state_t next =
        s_control.playback_status.state;
    if (audio_manager_playback_control_transition(
            s_control.playback_status.state,
            AUDIO_MANAGER_PLAYBACK_EVENT_PLAY_APPLIED,
            &next) == ESP_OK)
    {
        s_control.playback_status.state = next;
        s_control.playback_status.pause_reason =
            AUDIO_MANAGER_PLAYBACK_PAUSE_NONE;
    }
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_update_playback_position(uint64_t position_frames)
{
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.playback_status.resumable &&
        (position_frames <= s_control.playback_status.total_frames))
    {
        s_control.playback_status.position_frames = position_frames;
    }
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_update_playback_total(uint64_t total_frames)
{
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.playback_status.resumable)
    {
        s_control.playback_status.total_frames = total_frames;
    }
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_set_playback_paused(
    audio_manager_playback_pause_reason_t reason,
    uint64_t position_frames)
{
    portENTER_CRITICAL(&s_control_lock);
    audio_manager_playback_control_state_t next =
        s_control.playback_status.state;
    if (audio_manager_playback_control_transition(
            s_control.playback_status.state,
            AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_APPLIED,
            &next) == ESP_OK)
    {
        s_control.playback_status.state = next;
        s_control.playback_status.pause_reason = reason;
        s_control.playback_status.position_frames = position_frames;
    }
    s_control.pause_requested = false;
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_get_pause_metadata(
    audio_manager_playback_pause_reason_t *reason,
    uint32_t *generation)
{
    portENTER_CRITICAL(&s_control_lock);
    if (reason != NULL)
    {
        *reason = s_control.requested_pause_reason;
    }
    if (generation != NULL)
    {
        *generation = s_control.playback_status.generation;
    }
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_complete_playback_control(esp_err_t result)
{
    portENTER_CRITICAL(&s_control_lock);
    audio_manager_playback_control_state_t next =
        s_control.playback_status.state;
    if (result != ESP_OK)
    {
        if (audio_manager_playback_control_transition(
                s_control.playback_status.state,
                AUDIO_MANAGER_PLAYBACK_EVENT_FAILED,
                &next) == ESP_OK)
        {
            s_control.playback_status.state = next;
        }
    }

    next = s_control.playback_status.state;
    const audio_manager_playback_control_event_t terminal_event =
        (next == AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR)
            ? AUDIO_MANAGER_PLAYBACK_EVENT_RECOVERED
            : AUDIO_MANAGER_PLAYBACK_EVENT_FINISHED;
    if (audio_manager_playback_control_transition(
            next,
            terminal_event,
            &next) == ESP_OK)
    {
        s_control.playback_status.state = next;
    }
    s_control.playback_status.source = AUDIO_MANAGER_PLAYBACK_SOURCE_NONE;
    s_control.playback_status.pause_reason = AUDIO_MANAGER_PLAYBACK_PAUSE_NONE;
    s_control.playback_status.resumable = false;
    s_control.playback_status.generation = 0U;
    s_control.playback_status.position_frames = 0U;
    s_control.playback_status.total_frames = 0U;
    s_control.pause_requested = false;
    s_control.resume_requested = false;
    s_control.restart_requested = false;
    s_control.requested_pause_reason = AUDIO_MANAGER_PLAYBACK_PAUSE_NONE;
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_clear_resume_context(void)
{
    memset(&s_runtime.playback_resume, 0, sizeof(s_runtime.playback_resume));
}

static void audio_manager_restore_retained_playback_status(void)
{
    if (!s_runtime.playback_resume.valid)
    {
        return;
    }

    const audio_manager_playback_source_t source =
        (s_runtime.playback_resume.kind == AUDIO_PLAYBACK_SOURCE_WAV_PCM16)
            ? AUDIO_MANAGER_PLAYBACK_SOURCE_WAV
            : AUDIO_MANAGER_PLAYBACK_SOURCE_RECORDED;
    portENTER_CRITICAL(&s_control_lock);
    s_control.playback_status = (audio_manager_playback_status_t) {
        .state = AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED,
        .source = source,
        .pause_reason = s_runtime.playback_resume.pause_reason,
        .last_action = AUDIO_MANAGER_PLAYBACK_ACTION_PAUSE,
        .resumable = true,
        .generation = s_runtime.playback_resume.generation,
        .position_frames = s_runtime.playback_resume.position_frames,
        .total_frames = s_runtime.playback_resume.total_frames,
        .position_granularity_frames =
            AUDIO_MANAGER_PLAYBACK_POSITION_GRANULARITY_FRAMES,
        .last_control_result = ESP_OK,
    };
    portEXIT_CRITICAL(&s_control_lock);
}

static bool audio_manager_shutdown_is_requested(void)
{
    bool shutdown_requested;

    portENTER_CRITICAL(&s_control_lock);
    shutdown_requested = s_control.shutdown_requested;
    portEXIT_CRITICAL(&s_control_lock);

    return shutdown_requested;
}

static audio_record_stop_reason_t audio_manager_record_stop_reason(
    audio_record_control_t control)
{
    if (control == AUDIO_RECORD_CONTROL_GOLDEN_FIXED)
    {
        return AUDIO_RECORD_STOP_NONE;
    }

    bool shutdown_requested;
    bool cancel_requested;
    bool record_stop_requested;

    portENTER_CRITICAL(&s_control_lock);
    shutdown_requested = s_control.shutdown_requested;
    cancel_requested = s_control.cancel_requested;
    record_stop_requested = s_control.record_stop_requested;
    portEXIT_CRITICAL(&s_control_lock);

    if (shutdown_requested || cancel_requested)
    {
        return AUDIO_RECORD_STOP_ABORT;
    }

    if ((control == AUDIO_RECORD_CONTROL_MANUAL) && record_stop_requested)
    {
        return AUDIO_RECORD_STOP_MANUAL;
    }

    return AUDIO_RECORD_STOP_NONE;
}

static bool audio_manager_recorded_playback_cancel_enabled(void)
{
    bool enabled;

    portENTER_CRITICAL(&s_control_lock);
    enabled =
        (s_control.operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK);
    portEXIT_CRITICAL(&s_control_lock);

    return enabled;
}

static void audio_manager_finish_operation(void)
{
    portENTER_CRITICAL(&s_control_lock);
    s_control.operation = AUDIO_MANAGER_OPERATION_NONE;
    s_control.cancel_requested = false;
    s_control.record_stop_requested = false;
    s_control.pause_requested = false;
    s_control.resume_requested = false;
    s_control.restart_requested = false;
    s_control.requested_pause_reason = AUDIO_MANAGER_PLAYBACK_PAUSE_NONE;
    portEXIT_CRITICAL(&s_control_lock);
}

static void audio_manager_suspend_operation(void)
{
    portENTER_CRITICAL(&s_control_lock);
    s_control.operation = AUDIO_MANAGER_OPERATION_NONE;
    s_control.cancel_requested = false;
    s_control.record_stop_requested = false;
    s_control.pause_requested = false;
    /* Keep a resume requested while PAUSING until the released retained source
     * is actually eligible for queueing. Clearing it here used to make a
     * rapid pause->resume report success but leave the source paused forever. */
    s_control.restart_requested = false;
    portEXIT_CRITICAL(&s_control_lock);
}

static bool audio_manager_consume_resume_requested(void)
{
    bool requested = false;
    portENTER_CRITICAL(&s_control_lock);
    requested = s_control.resume_requested;
    s_control.resume_requested = false;
    portEXIT_CRITICAL(&s_control_lock);
    return requested;
}

static void audio_manager_finish_wav_operation(void)
{
    audio_manager_finish_operation();
}

/**
 * @brief Reserve the manager slot for exactly one golden cycle.
 *
 * Mixed stress uses this only after its coordinator has had a chance to queue
 * a WAV command. The manager task remains the sole I2S and source owner.
 */
static bool audio_manager_try_begin_stability_operation(void)
{
    bool started = false;

    portENTER_CRITICAL(&s_control_lock);
    if (s_control.task_running &&
        !s_control.shutdown_requested &&
        (s_control.operation == AUDIO_MANAGER_OPERATION_NONE))
    {
        s_control.operation = AUDIO_MANAGER_OPERATION_STABILITY;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
        started = true;
    }
    portEXIT_CRITICAL(&s_control_lock);

    return started;
}

static void audio_manager_record_rx_io(
    esp_err_t result,
    size_t bytes_read,
    int64_t duration_us)
{
    const uint32_t bounded_duration_us =
        (duration_us <= 0)
            ? 0U
            : ((uint64_t)duration_us > UINT32_MAX)
                ? UINT32_MAX
                : (uint32_t)duration_us;

    portENTER_CRITICAL(&s_diagnostics_lock);
    s_diagnostics.rx_bytes_requested += sizeof(s_rx_block);
    s_diagnostics.rx_bytes_read += bytes_read;

    if (bounded_duration_us > s_diagnostics.max_rx_read_duration_us)
    {
        s_diagnostics.max_rx_read_duration_us = bounded_duration_us;
    }

    if (result == ESP_ERR_TIMEOUT)
    {
        ++s_diagnostics.rx_timeout_count;
    }

    portEXIT_CRITICAL(&s_diagnostics_lock);
}

static void audio_manager_record_tx_io(
    esp_err_t result,
    size_t bytes_requested,
    size_t bytes_written,
    int64_t duration_us)
{
    const uint32_t bounded_duration_us =
        (duration_us <= 0)
            ? 0U
            : ((uint64_t)duration_us > UINT32_MAX)
                ? UINT32_MAX
                : (uint32_t)duration_us;

    portENTER_CRITICAL(&s_diagnostics_lock);
    s_diagnostics.tx_bytes_requested += bytes_requested;
    s_diagnostics.tx_bytes_written += bytes_written;

    if (bounded_duration_us > s_diagnostics.max_tx_write_duration_us)
    {
        s_diagnostics.max_tx_write_duration_us = bounded_duration_us;
    }

    if (result == ESP_ERR_TIMEOUT)
    {
        ++s_diagnostics.tx_timeout_count;
    }

    if (bytes_written != bytes_requested)
    {
        ++s_diagnostics.tx_partial_write_count;
    }

    portEXIT_CRITICAL(&s_diagnostics_lock);
}

static void audio_manager_refresh_diagnostics_locked(void)
{
    audio_manager_diagnostics_t diagnostics = {0};
    audio_manager_snapshot_diagnostics(&diagnostics);

    s_runtime.status.capture_i2s_active = diagnostics.capture_i2s_active;
    s_runtime.status.playback_i2s_active = diagnostics.playback_i2s_active;
    s_runtime.status.recorded_audio_available = s_runtime.recorded_audio_valid;
    s_runtime.status.rx_bytes_requested = diagnostics.rx_bytes_requested;
    s_runtime.status.rx_bytes_read = diagnostics.rx_bytes_read;
    s_runtime.status.tx_bytes_requested = diagnostics.tx_bytes_requested;
    s_runtime.status.tx_bytes_written = diagnostics.tx_bytes_written;
    s_runtime.status.rx_overflow_count = diagnostics.rx_overflow_count;
    s_runtime.status.rx_timeout_count = diagnostics.rx_timeout_count;
    s_runtime.status.tx_queue_overflow_count =
        diagnostics.tx_queue_overflow_count;
    s_runtime.status.tx_timeout_count = diagnostics.tx_timeout_count;
    s_runtime.status.tx_partial_write_count =
        diagnostics.tx_partial_write_count;
    s_runtime.status.max_rx_read_duration_us =
        diagnostics.max_rx_read_duration_us;
    s_runtime.status.max_tx_write_duration_us =
        diagnostics.max_tx_write_duration_us;

    if (s_runtime.task_handle != NULL)
    {
        s_runtime.status.task_stack_high_water_bytes =
            uxTaskGetStackHighWaterMark(s_runtime.task_handle);
    }
}

static void audio_manager_notify_status_changed(void)
{
    if (s_runtime.status_mutex == NULL)
    {
        return;
    }

    audio_manager_status_t status_snapshot;
    audio_manager_status_callback_t callback;
    void *callback_context;

    if (!audio_manager_take_status_mutex("publishing status"))
    {
        return;
    }

    audio_manager_refresh_diagnostics_locked();
    status_snapshot = s_runtime.status;
    callback = s_runtime.status_callback;
    callback_context = s_runtime.status_callback_context;

    xSemaphoreGive(s_runtime.status_mutex);

    /* Never invoke application code while holding the manager mutex. */
    if (callback != NULL)
    {
        callback(&status_snapshot, callback_context);
    }
}

static void audio_manager_set_state(audio_manager_state_t state)
{
    if (s_runtime.status_mutex == NULL)
    {
        return;
    }

    bool changed = false;

    if (!audio_manager_take_status_mutex("updating state"))
    {
        return;
    }

    if (s_runtime.status.state != state)
    {
        s_runtime.status.state = state;
        changed = true;
    }

    xSemaphoreGive(s_runtime.status_mutex);

    if (changed)
    {
        APP_LOGD(TAG, STATE_S_44CF8134, "State -> %s", audio_manager_state_to_string(state));
        audio_manager_notify_status_changed();
    }
}

/* Static Functions: I2S Callbacks ----------------------------------------- */
static bool IRAM_ATTR audio_manager_rx_overflow_callback(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_context)
{
    (void)handle;
    (void)event;
    (void)user_context;
    portENTER_CRITICAL_ISR(&s_diagnostics_lock);
    ++s_diagnostics.rx_overflow_count;
    portEXIT_CRITICAL_ISR(&s_diagnostics_lock);
    return false;
}

static bool IRAM_ATTR audio_manager_tx_overflow_callback(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_context)
{
    (void)handle;
    (void)event;
    (void)user_context;
    portENTER_CRITICAL_ISR(&s_diagnostics_lock);
    ++s_diagnostics.tx_queue_overflow_count;
    portEXIT_CRITICAL_ISR(&s_diagnostics_lock);
    return false;
}

/* Static Functions: Resource Diagnostics --------------------------------- */
static void log_heap_state(const char *label)
{
    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_min =
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const size_t dma_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const size_t psram_free =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_min =
        heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    APP_LOGI(
        TAG, HEAP_S_INTERNAL_U_MIN_74CF414A,
        "HEAP[%s] internal=%u min=%u largest=%u dma=%u min=%u largest=%u psram=%u min=%u largest=%u",
        (label != NULL) ? label : "?",
        (unsigned)internal_free,
        (unsigned)internal_min,
        (unsigned)internal_largest,
        (unsigned)dma_free,
        (unsigned)dma_min,
        (unsigned)dma_largest,
        (unsigned)psram_free,
        (unsigned)psram_min,
        (unsigned)psram_largest);
}

/* Static Functions: RX / INMP441 ------------------------------------------ */
static esp_err_t hold_amplifier_data_low(void)
{
    esp_err_t result = gpio_reset_pin(AUDIO_GPIO_SPK_DOUT);
    if (result != ESP_OK)
    {
        return result;
    }

    result = gpio_set_direction(AUDIO_GPIO_SPK_DOUT, GPIO_MODE_OUTPUT);
    if (result != ESP_OK)
    {
        return result;
    }

    return gpio_set_level(AUDIO_GPIO_SPK_DOUT, 0);
}

static esp_err_t start_i2s_rx(void)
{
    if (s_runtime.rx_channel != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_MANAGER_DMA_DESC_NUM;
    channel_config.dma_frame_num = AUDIO_MANAGER_FRAMES_PER_BLOCK;

    esp_err_t result = i2s_new_channel(
        &channel_config,
        NULL,
        &s_runtime.rx_channel);
    if (result != ESP_OK)
    {
        s_runtime.rx_channel = NULL;
        return result;
    }

    const i2s_event_callbacks_t callbacks = {
        .on_recv = NULL,
        .on_recv_q_ovf = audio_manager_rx_overflow_callback,
        .on_sent = NULL,
        .on_send_q_ovf = NULL,
    };

    result = i2s_channel_register_event_callback(
        s_runtime.rx_channel,
        &callbacks,
        NULL);
    if (result != ESP_OK)
    {
        (void)i2s_del_channel(s_runtime.rx_channel);
        s_runtime.rx_channel = NULL;
        return result;
    }

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_MANAGER_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_GPIO_BCLK,
            .ws = AUDIO_GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_GPIO_MIC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    result = i2s_channel_init_std_mode(s_runtime.rx_channel, &config);
    if (result == ESP_OK)
    {
        result = i2s_channel_enable(s_runtime.rx_channel);
        s_runtime.rx_enabled = (result == ESP_OK);
        audio_manager_set_capture_i2s_active(s_runtime.rx_enabled);
        if (s_runtime.rx_enabled)
        {
            /* This is the allocation boundary shared with the first Xiaozhi
             * Opus TLS write; retain one compact snapshot per capture. */
            log_heap_state("capture_i2s_enabled");
        }
    }

    if (result != ESP_OK)
    {
        if (s_runtime.rx_enabled)
        {
            (void)i2s_channel_disable(s_runtime.rx_channel);
        }
        (void)i2s_del_channel(s_runtime.rx_channel);
        s_runtime.rx_channel = NULL;
        s_runtime.rx_enabled = false;
        audio_manager_set_capture_i2s_active(false);
    }

    return result;
}

static esp_err_t stop_i2s_rx(void)
{
    if (s_runtime.rx_channel == NULL)
    {
        s_runtime.rx_enabled = false;
        audio_manager_set_capture_i2s_active(false);
        return ESP_OK;
    }

    esp_err_t first_error = ESP_OK;

    if (s_runtime.rx_enabled)
    {
        const esp_err_t disable_result =
            i2s_channel_disable(s_runtime.rx_channel);
        if (disable_result != ESP_OK)
        {
            first_error = disable_result;
        }
    }

    s_runtime.rx_enabled = false;
    audio_manager_set_capture_i2s_active(false);
    const esp_err_t delete_result = i2s_del_channel(s_runtime.rx_channel);
    s_runtime.rx_channel = NULL;

    return (first_error != ESP_OK) ? first_error : delete_result;
}

static esp_err_t read_rx_block(size_t *frames_read)
{
    if (frames_read == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *frames_read = 0U;
    size_t bytes_read = 0U;
    const int64_t start_us = esp_timer_get_time();

    /* ESP-IDF new I2S channel APIs take timeout directly in milliseconds. */
    const esp_err_t result = i2s_channel_read(
        s_runtime.rx_channel,
        s_rx_block,
        sizeof(s_rx_block),
        &bytes_read,
        AUDIO_MANAGER_I2S_TIMEOUT_MS);

    const int64_t duration_us = esp_timer_get_time() - start_us;
    audio_manager_record_rx_io(result, bytes_read, duration_us);

    if (result != ESP_OK)
    {
        return result;
    }

    const size_t bytes_per_frame =
        AUDIO_MANAGER_SLOT_COUNT * sizeof(int32_t);
    if ((bytes_read % bytes_per_frame) != 0U)
    {
        APP_LOGW(
            TAG, RX_BYTE_COUNT_IS_NOT_B78C9762,
            "RX byte count is not frame aligned: %u",
            (unsigned)bytes_read);
    }

    *frames_read = bytes_read / bytes_per_frame;
    return ESP_OK;
}

static esp_err_t discard_microphone_startup(
    audio_record_control_t control,
    audio_record_stop_reason_t *stop_reason)
{
    if (stop_reason == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *stop_reason = AUDIO_RECORD_STOP_NONE;
    for (uint32_t block = 0U;
         block < AUDIO_MANAGER_STARTUP_DISCARD_BLOCKS;
         ++block)
    {
        *stop_reason = audio_manager_record_stop_reason(control);
        if (*stop_reason != AUDIO_RECORD_STOP_NONE)
        {
            return ESP_OK;
        }

        size_t frames_read = 0U;
        const esp_err_t result = read_rx_block(&frames_read);
        if (result != ESP_OK)
        {
            return result;
        }
    }

    return ESP_OK;
}

static void update_slot_stats(slot_stats_t *stats, int32_t sample)
{
    if (stats == NULL)
    {
        return;
    }

    const uint32_t magnitude =
        (sample < 0) ? (uint32_t)(-(int64_t)sample) : (uint32_t)sample;

    stats->absolute_sum += magnitude;
    ++stats->sample_count;
    if (magnitude > stats->peak)
    {
        stats->peak = magnitude;
    }
}

static esp_err_t detect_microphone_slot(
    microphone_slot_t *selected_slot,
    audio_record_control_t control,
    audio_record_stop_reason_t *stop_reason)
{
    if ((selected_slot == NULL) || (stop_reason == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    slot_stats_t left = {0};
    slot_stats_t right = {0};
    *stop_reason = AUDIO_RECORD_STOP_NONE;

    for (uint32_t block = 0U;
         block < AUDIO_MANAGER_SLOT_DETECT_BLOCKS;
         ++block)
    {
        *stop_reason = audio_manager_record_stop_reason(control);
        if (*stop_reason != AUDIO_RECORD_STOP_NONE)
        {
            return ESP_OK;
        }

        size_t frames_read = 0U;
        const esp_err_t result = read_rx_block(&frames_read);
        if (result != ESP_OK)
        {
            return result;
        }

        for (size_t frame = 0U; frame < frames_read; ++frame)
        {
            const size_t base = frame * AUDIO_MANAGER_SLOT_COUNT;
            update_slot_stats(
                &left,
                audio_manager_stream_convert_raw_slot_to_pcm24(s_rx_block[base]));
            update_slot_stats(
                &right,
                audio_manager_stream_convert_raw_slot_to_pcm24(s_rx_block[base + 1U]));
        }
    }

    const uint64_t left_average =
        (left.sample_count == 0U) ? 0U : left.absolute_sum / left.sample_count;
    const uint64_t right_average =
        (right.sample_count == 0U) ? 0U : right.absolute_sum / right.sample_count;

    *selected_slot =
        (right_average > left_average)
            ? MICROPHONE_SLOT_RIGHT
            : MICROPHONE_SLOT_LEFT;

    APP_LOGI(
        TAG, SLOT_DETECT_LEFT_AVG_LLU_D7D37C52,
        "SLOT_DETECT left_avg=%llu left_peak=%u right_avg=%llu right_peak=%u selected=%s",
        (unsigned long long)left_average,
        (unsigned)left.peak,
        (unsigned long long)right_average,
        (unsigned)right.peak,
        (*selected_slot == MICROPHONE_SLOT_LEFT) ? "LEFT" : "RIGHT");

    return ESP_OK;
}

static esp_err_t record_audio(
    microphone_slot_t selected_slot,
    size_t target_sample_count,
    audio_record_control_t control,
    size_t *samples_recorded,
    audio_record_stop_reason_t *stop_reason)
{
    if ((samples_recorded == NULL) ||
        (stop_reason == NULL) ||
        (s_runtime.recording_pcm24 == NULL) ||
        (target_sample_count == 0U) ||
        (target_sample_count > s_runtime.sample_capacity))
    {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t selected_slot_index =
        (selected_slot == MICROPHONE_SLOT_RIGHT) ? 1U : 0U;

    size_t captured = 0U;
    size_t next_progress = AUDIO_MANAGER_SAMPLE_RATE_HZ;
    *stop_reason = AUDIO_RECORD_STOP_NONE;

    while (captured < target_sample_count)
    {
        *stop_reason = audio_manager_record_stop_reason(control);
        if (*stop_reason != AUDIO_RECORD_STOP_NONE)
        {
            break;
        }

        size_t frames_read = 0U;
        const esp_err_t result = read_rx_block(&frames_read);
        if (result != ESP_OK)
        {
            *samples_recorded = captured;
            return result;
        }

        const size_t remaining = target_sample_count - captured;
        if (frames_read > remaining)
        {
            frames_read = remaining;
        }

        for (size_t frame = 0U; frame < frames_read; ++frame)
        {
            const size_t source_index =
                (frame * AUDIO_MANAGER_SLOT_COUNT) + selected_slot_index;
            s_runtime.recording_pcm24[captured + frame] =
                audio_manager_stream_convert_raw_slot_to_pcm24(s_rx_block[source_index]);
        }

        captured += frames_read;

        while ((captured >= next_progress) &&
               (next_progress <= target_sample_count))
        {
            /* Fixed/manual recorder progress is useful outside live Xiaozhi
             * turns, but synchronous console output is unnecessary during the
             * capture-to-uplink path. The turn summary preserves diagnostics
             * after release without adding one log write per captured second. */
            if (!voice_recording_critical_is_active())
            {
                APP_LOGI(
                    TAG, RECORDED_U_U_SECONDS_6D1F33DF,
                    "Recorded %u/%u seconds",
                    (unsigned)(next_progress / AUDIO_MANAGER_SAMPLE_RATE_HZ),
                    (unsigned)(target_sample_count / AUDIO_MANAGER_SAMPLE_RATE_HZ));
            }
            next_progress += AUDIO_MANAGER_SAMPLE_RATE_HZ;
        }
    }

    *samples_recorded = captured;
    return ESP_OK;
}

/* Static Functions: TX / MAX98357A ---------------------------------------- */
static esp_err_t start_i2s_tx(void)
{
    if (s_runtime.tx_channel != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_MANAGER_DMA_DESC_NUM;
    channel_config.dma_frame_num = AUDIO_MANAGER_FRAMES_PER_BLOCK;

    esp_err_t result = i2s_new_channel(
        &channel_config,
        &s_runtime.tx_channel,
        NULL);
    if (result != ESP_OK)
    {
        s_runtime.tx_channel = NULL;
        return result;
    }

    const i2s_event_callbacks_t callbacks = {
        .on_recv = NULL,
        .on_recv_q_ovf = NULL,
        .on_sent = NULL,
        .on_send_q_ovf = audio_manager_tx_overflow_callback,
    };

    result = i2s_channel_register_event_callback(
        s_runtime.tx_channel,
        &callbacks,
        NULL);
    if (result != ESP_OK)
    {
        (void)i2s_del_channel(s_runtime.tx_channel);
        s_runtime.tx_channel = NULL;
        return result;
    }

    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_MANAGER_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_GPIO_BCLK,
            .ws = AUDIO_GPIO_WS,
            .dout = AUDIO_GPIO_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;

    result = i2s_channel_init_std_mode(s_runtime.tx_channel, &config);
    if (result != ESP_OK)
    {
        (void)i2s_del_channel(s_runtime.tx_channel);
        s_runtime.tx_channel = NULL;
        return result;
    }

    /* Proven behavior: preload DMA with silence before enabling TX clocks. */
    size_t loaded = sizeof(s_silence_block);
    while (loaded == sizeof(s_silence_block))
    {
        loaded = 0U;
        result = i2s_channel_preload_data(
            s_runtime.tx_channel,
            s_silence_block,
            sizeof(s_silence_block),
            &loaded);
        if (result != ESP_OK)
        {
            (void)i2s_del_channel(s_runtime.tx_channel);
            s_runtime.tx_channel = NULL;
            return result;
        }
    }

    result = i2s_channel_enable(s_runtime.tx_channel);
    s_runtime.tx_enabled = (result == ESP_OK);
    audio_manager_set_playback_i2s_active(s_runtime.tx_enabled);

    if (result != ESP_OK)
    {
        (void)i2s_del_channel(s_runtime.tx_channel);
        s_runtime.tx_channel = NULL;
        s_runtime.tx_enabled = false;
        audio_manager_set_playback_i2s_active(false);
    }

    return result;
}

static esp_err_t stop_i2s_tx(void)
{
    if (s_runtime.tx_channel == NULL)
    {
        s_runtime.tx_enabled = false;
        audio_manager_set_playback_i2s_active(false);
        return hold_amplifier_data_low();
    }

    esp_err_t first_error = ESP_OK;

    if (s_runtime.tx_enabled)
    {
        const esp_err_t disable_result =
            i2s_channel_disable(s_runtime.tx_channel);
        if (disable_result != ESP_OK)
        {
            first_error = disable_result;
        }
    }

    s_runtime.tx_enabled = false;
    audio_manager_set_playback_i2s_active(false);
    const esp_err_t delete_result = i2s_del_channel(s_runtime.tx_channel);
    s_runtime.tx_channel = NULL;
    const esp_err_t gpio_result = hold_amplifier_data_low();

    if (first_error != ESP_OK)
    {
        return first_error;
    }
    if (delete_result != ESP_OK)
    {
        return delete_result;
    }

    return gpio_result;
}

static esp_err_t write_tx_frames(size_t frame_count)
{
    if ((frame_count == 0U) ||
        (frame_count > AUDIO_MANAGER_FRAMES_PER_BLOCK))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes_to_write =
        frame_count * AUDIO_MANAGER_SLOT_COUNT * sizeof(int16_t);
    size_t bytes_written = 0U;
    const int64_t start_us = esp_timer_get_time();

    /* ESP-IDF new I2S channel APIs take timeout directly in milliseconds. */
    const esp_err_t result = i2s_channel_write(
        s_runtime.tx_channel,
        s_tx_block,
        bytes_to_write,
        &bytes_written,
        AUDIO_MANAGER_I2S_TIMEOUT_MS);

    const int64_t duration_us = esp_timer_get_time() - start_us;
    audio_manager_record_tx_io(
        result,
        bytes_to_write,
        bytes_written,
        duration_us);

    if (result != ESP_OK)
    {
        return result;
    }

    return (bytes_written == bytes_to_write) ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_silence_blocks(
    uint32_t block_count,
    bool *cancelled)
{
    memset(s_tx_block, 0, sizeof(s_tx_block));

    for (uint32_t block = 0U; block < block_count; ++block)
    {
        if ((cancelled != NULL) && audio_manager_cancel_is_requested())
        {
            *cancelled = true;
            break;
        }

        const esp_err_t result =
            write_tx_frames(AUDIO_MANAGER_FRAMES_PER_BLOCK);
        if (result != ESP_OK)
        {
            return result;
        }
    }

    return ESP_OK;
}

static esp_err_t write_controlled_silence_blocks(
    uint32_t block_count,
    audio_playback_flow_t *flow)
{
    if (flow == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_tx_block, 0, sizeof(s_tx_block));
    for (uint32_t block = 0U; block < block_count; ++block)
    {
        *flow = audio_manager_playback_flow_requested();
        if (*flow != AUDIO_PLAYBACK_FLOW_CONTINUE)
        {
            break;
        }

        const esp_err_t result =
            write_tx_frames(AUDIO_MANAGER_FRAMES_PER_BLOCK);
        if (result != ESP_OK)
        {
            return result;
        }
    }
    return ESP_OK;
}

static int32_t apply_playback_volume_percent(int32_t sample_pcm24)
{
    return (int32_t)(
        ((int64_t)sample_pcm24 * s_runtime.config.playback_volume_percent) /
        AUDIO_DSP_VOLUME_PERCENT_MAX);
}

static int16_t decode_wav_pcm16_le(const uint8_t *sample_bytes)
{
    const uint16_t raw_sample =
        (uint16_t)((uint16_t)sample_bytes[0] |
                   ((uint16_t)sample_bytes[1] << 8U));
    const int32_t signed_sample =
        (raw_sample <= (uint16_t)INT16_MAX)
            ? (int32_t)raw_sample
            : (int32_t)raw_sample - 65536;
    return (int16_t)signed_sample;
}

static int16_t apply_wav_volume_percent(int16_t sample_pcm16)
{
    const int32_t scaled =
        ((int32_t)sample_pcm16 *
         (int32_t)s_runtime.config.playback_volume_percent) /
        (int32_t)AUDIO_DSP_VOLUME_PERCENT_MAX;

    if (scaled > (int32_t)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16)
    {
        return (int16_t)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16;
    }
    if (scaled < -(int32_t)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16)
    {
        return (int16_t)-(int32_t)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16;
    }

    return (int16_t)scaled;
}

static esp_err_t play_recording(
    size_t start_sample_index,
    size_t sample_count,
    audio_dsp_playback_stats_t *stats,
    audio_playback_flow_t *flow,
    size_t *next_sample_index)
{
    if ((s_runtime.recording_pcm24 == NULL) ||
        (sample_count == 0U) ||
        (start_sample_index > sample_count) ||
        (stats == NULL) || (flow == NULL) || (next_sample_index == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (start_sample_index == 0U)
    {
        memset(stats, 0, sizeof(*stats));
    }
    *flow = AUDIO_PLAYBACK_FLOW_CONTINUE;
    size_t sample_index = start_sample_index;
    *next_sample_index = sample_index;

    while (sample_index < sample_count)
    {
        *flow = audio_manager_playback_flow_requested();
        if (*flow != AUDIO_PLAYBACK_FLOW_CONTINUE)
        {
            break;
        }

        size_t frames = sample_count - sample_index;
        if (frames > AUDIO_MANAGER_FRAMES_PER_BLOCK)
        {
            frames = AUDIO_MANAGER_FRAMES_PER_BLOCK;
        }

        for (size_t frame = 0U; frame < frames; ++frame)
        {
            bool soft_compressed = false;
            bool limited = false;
            const int32_t volume_scaled_pcm24 =
                apply_playback_volume_percent(
                    s_runtime.recording_pcm24[sample_index + frame]);

            const int16_t mono_sample = audio_dsp_prepare_output_sample(
                volume_scaled_pcm24,
                sample_index + frame,
                sample_count,
                &soft_compressed,
                &limited);

            const uint32_t magnitude =
                (mono_sample < 0)
                    ? (uint32_t)(-(int32_t)mono_sample)
                    : (uint32_t)mono_sample;

            stats->absolute_sum += magnitude;
            ++stats->sample_count;
            if (magnitude > stats->peak)
            {
                stats->peak = magnitude;
            }
            if (soft_compressed)
            {
                ++stats->soft_compressed_samples;
            }
            if (limited)
            {
                ++stats->limited_samples;
            }

            const size_t slot = frame * AUDIO_MANAGER_SLOT_COUNT;
            s_tx_block[slot] = mono_sample;
            s_tx_block[slot + 1U] = mono_sample;
        }

        const esp_err_t result = write_tx_frames(frames);
        if (result != ESP_OK)
        {
            return result;
        }

        sample_index += frames;
        *next_sample_index = sample_index;
        audio_manager_update_playback_position(sample_index);
    }

    return ESP_OK;
}

/**
 * @brief Wait for the next producer-ready WAV block without touching I2S.
 *
 * The initial wait is intentionally completed before TX starts.  A later
 * wait means the consumer reached a 10-second boundary before the reader had
 * another cache block ready; it is diagnosed as software prefetch starvation,
 * not as a hardware I2S-underrun measurement.
 */
static esp_err_t audio_manager_take_prefetched_wav_item(
    audio_wav_prefetch_t *prefetch,
    audio_wav_prefetch_item_t *item,
    audio_wav_playback_metrics_t *metrics,
    bool initial_wait,
    audio_playback_flow_t *flow)
{
    if ((prefetch == NULL) || (item == NULL) || (metrics == NULL) ||
        (flow == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t wait_start_us = esp_timer_get_time();
    while (true)
    {
        *flow = audio_manager_playback_flow_requested();
        if (*flow != AUDIO_PLAYBACK_FLOW_CONTINUE)
        {
            return ESP_OK;
        }

        if (audio_wav_prefetch_take_ready(
                prefetch,
                item,
                pdMS_TO_TICKS(AUDIO_MANAGER_WAV_PREFETCH_WAIT_POLL_MS)) ==
            pdTRUE)
        {
            const int64_t wait_us = esp_timer_get_time() - wait_start_us;
            const uint32_t wait_ms =
                (wait_us <= 0)
                    ? 0U
                    : ((uint64_t)wait_us / 1000U > UINT32_MAX)
                        ? UINT32_MAX
                        : (uint32_t)((uint64_t)wait_us / 1000U);

            if (initial_wait)
            {
                metrics->initial_prefetch_wait_ms = wait_ms;
            }
            else
            {
                metrics->prefetch_wait_ms += wait_ms;
            }

            if (item->result != ESP_OK)
            {
                return item->result;
            }

            if ((item->slot_index >= AUDIO_WAV_PREFETCH_SLOT_COUNT) ||
                (item->valid_bytes == 0U) ||
                ((item->valid_bytes % sizeof(int16_t)) != 0U) ||
                (audio_wav_prefetch_slot_data(prefetch, item->slot_index) ==
                 NULL))
            {
                return ESP_ERR_INVALID_RESPONSE;
            }

            return ESP_OK;
        }

        if (!initial_wait)
        {
            ++metrics->prefetch_starvation_count;
            metrics->prefetch_wait_ms +=
                AUDIO_MANAGER_WAV_PREFETCH_WAIT_POLL_MS;
            APP_LOGE(
                TAG, WAV_PREFETCH_STARVATION_NO_R_999126B7,
                "WAV prefetch starvation: no READY block after %ums",
                (unsigned)AUDIO_MANAGER_WAV_PREFETCH_WAIT_POLL_MS);
            return ESP_ERR_TIMEOUT;
        }

        if (audio_wav_prefetch_wait_stopped(prefetch, 0U) == ESP_OK)
        {
            const esp_err_t worker_result =
                audio_wav_prefetch_get_worker_result(prefetch);
            return (worker_result == ESP_OK)
                       ? ESP_ERR_INVALID_SIZE
                       : worker_result;
        }
    }
}

/** @brief Convert one immutable READY PCM16 block into proven TX frames. */
static esp_err_t audio_manager_play_prefetched_wav_item(
    const audio_wav_prefetch_t *prefetch,
    const audio_wav_prefetch_item_t *item,
    audio_wav_playback_metrics_t *metrics,
    audio_playback_flow_t *flow)
{
    if ((prefetch == NULL) || (item == NULL) || (metrics == NULL) ||
        (flow == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *const pcm_bytes = audio_wav_prefetch_slot_data(
        prefetch,
        item->slot_index);
    if ((pcm_bytes == NULL) || (item->valid_bytes == 0U) ||
        ((item->valid_bytes % sizeof(int16_t)) != 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t sample_count = item->valid_bytes / sizeof(int16_t);
    size_t sample_offset = 0U;

    while (sample_offset < sample_count)
    {
        *flow = audio_manager_playback_flow_requested();
        if (*flow != AUDIO_PLAYBACK_FLOW_CONTINUE)
        {
            return ESP_OK;
        }

        size_t frames = sample_count - sample_offset;
        if (frames > AUDIO_MANAGER_FRAMES_PER_BLOCK)
        {
            frames = AUDIO_MANAGER_FRAMES_PER_BLOCK;
        }

        for (size_t frame = 0U; frame < frames; ++frame)
        {
            const size_t sample_byte_offset =
                (sample_offset + frame) * sizeof(int16_t);
            const int16_t fixed_scaled_sample =
                audio_wav_pcm16_scale_full_range_to_peak(
                    decode_wav_pcm16_le(&pcm_bytes[sample_byte_offset]),
                    AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
            const int16_t mono_sample = apply_wav_volume_percent(
                fixed_scaled_sample);
            const uint32_t magnitude =
                (mono_sample < 0)
                    ? (uint32_t)(-(int32_t)mono_sample)
                    : (uint32_t)mono_sample;
            if (magnitude > metrics->output_peak_pcm16)
            {
                metrics->output_peak_pcm16 = magnitude;
            }
            const size_t slot = frame * AUDIO_MANAGER_SLOT_COUNT;
            s_tx_block[slot] = mono_sample;
            s_tx_block[slot + 1U] = mono_sample;
        }

        const esp_err_t result = write_tx_frames(frames);
        if (result != ESP_OK)
        {
            return result;
        }

        metrics->data_bytes_streamed += frames * sizeof(int16_t);
        sample_offset += frames;
        audio_manager_update_playback_position(
            metrics->data_bytes_streamed / sizeof(int16_t));
    }

    return ESP_OK;
}

static esp_err_t play_wav_stream(
    audio_wav_prefetch_t *prefetch,
    uint64_t start_data_offset,
    audio_wav_playback_metrics_t *metrics,
    audio_playback_flow_t *flow,
    uint64_t *next_data_offset,
    audio_wav_info_t *retained_info,
    bool *retained_info_valid)
{
    if ((prefetch == NULL) || (metrics == NULL) || (flow == NULL) ||
        (next_data_offset == NULL) || (retained_info == NULL) ||
        (retained_info_valid == NULL) ||
        !audio_wav_prefetch_is_active(prefetch))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(metrics, 0, sizeof(*metrics));
    metrics->fixed_scale_gain_q16 =
        AUDIO_WAV_PCM16_FULL_SCALE_GAIN_Q16(
            AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
    metrics->data_bytes_streamed = start_data_offset;
    *flow = AUDIO_PLAYBACK_FLOW_CONTINUE;
    *next_data_offset = start_data_offset;
    *retained_info_valid = false;
    metrics->prefetch_block_bytes = AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES;

    audio_wav_prefetch_item_t item = {0};
    bool item_held = false;
    esp_err_t result = audio_manager_take_prefetched_wav_item(
        prefetch,
        &item,
        metrics,
        true,
        flow);
    if ((result != ESP_OK) || (*flow != AUDIO_PLAYBACK_FLOW_CONTINUE))
    {
        return result;
    }
    item_held = true;

    audio_wav_info_t info = {0};
    result = audio_wav_prefetch_get_info(prefetch, &info);
    if (result == ESP_OK)
    {
        metrics->expected_data_bytes = info.data_size_bytes;
        metrics->expected_duration_ms = info.duration_ms;
        *retained_info = info;
        *retained_info_valid = true;
        audio_manager_update_playback_total(
            info.data_size_bytes / sizeof(int16_t));
    }

    if (result == ESP_OK)
    {
        *flow = audio_manager_playback_flow_requested();
    }

    bool tx_started = false;
    if ((result == ESP_OK) && (*flow == AUDIO_PLAYBACK_FLOW_CONTINUE))
    {
        result = start_i2s_tx();
        tx_started = (result == ESP_OK);
    }

    if ((result == ESP_OK) && (*flow == AUDIO_PLAYBACK_FLOW_CONTINUE))
    {
        result = write_controlled_silence_blocks(
            AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS,
            flow);
    }

    int64_t playback_start_us = 0;
    if ((result == ESP_OK) && (*flow == AUDIO_PLAYBACK_FLOW_CONTINUE))
    {
        playback_start_us = esp_timer_get_time();
        audio_manager_set_state(AUDIO_MANAGER_STATE_PLAYBACK);
        audio_manager_set_playback_applied();
    }

    while ((result == ESP_OK) &&
           (*flow == AUDIO_PLAYBACK_FLOW_CONTINUE) && item_held)
    {
        result = audio_manager_play_prefetched_wav_item(
            prefetch,
            &item,
            metrics,
            flow);

        *next_data_offset = metrics->data_bytes_streamed;

        const bool final_block = item.final_block;
        const esp_err_t release_result = audio_wav_prefetch_release_slot(
            prefetch,
            item.slot_index);
        item_held = false;
        if ((result == ESP_OK) && (release_result != ESP_OK))
        {
            result = release_result;
        }

        if ((result != ESP_OK) ||
            (*flow != AUDIO_PLAYBACK_FLOW_CONTINUE))
        {
            break;
        }

        if (metrics->data_bytes_streamed == metrics->expected_data_bytes)
        {
            if (!final_block)
            {
                result = ESP_ERR_INVALID_SIZE;
            }
            break;
        }

        if (final_block ||
            (metrics->data_bytes_streamed > metrics->expected_data_bytes))
        {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }

        result = audio_manager_take_prefetched_wav_item(
            prefetch,
            &item,
            metrics,
            false,
            flow);
        item_held = (result == ESP_OK) &&
                    (*flow == AUDIO_PLAYBACK_FLOW_CONTINUE);
    }

    if (item_held)
    {
        const esp_err_t release_result = audio_wav_prefetch_release_slot(
            prefetch,
            item.slot_index);
        if ((result == ESP_OK) && (release_result != ESP_OK))
        {
            result = release_result;
        }
    }

    if ((result == ESP_OK) &&
        (*flow == AUDIO_PLAYBACK_FLOW_CONTINUE) &&
        (metrics->data_bytes_streamed != metrics->expected_data_bytes))
    {
        result = ESP_ERR_INVALID_SIZE;
    }

    if ((result == ESP_OK) &&
        (*flow == AUDIO_PLAYBACK_FLOW_CONTINUE))
    {
        bool terminal_cancelled = false;
        result = write_silence_blocks(
            AUDIO_MANAGER_POST_PLAYBACK_SILENCE_BLOCKS,
            &terminal_cancelled);
        if (terminal_cancelled)
        {
            *flow = AUDIO_PLAYBACK_FLOW_CANCEL;
        }
    }

    if (playback_start_us > 0)
    {
        const int64_t elapsed_us = esp_timer_get_time() - playback_start_us;
        metrics->playback_elapsed_ms =
            (elapsed_us <= 0)
                ? 0U
                : ((uint64_t)elapsed_us / 1000U > UINT32_MAX)
                    ? UINT32_MAX
                    : (uint32_t)((uint64_t)elapsed_us / 1000U);
    }

    if (tx_started)
    {
        const esp_err_t stop_result = stop_i2s_tx();
        if ((result == ESP_OK) && (stop_result != ESP_OK))
        {
            result = stop_result;
        }
    }

    return result;
}

/**
 * @brief Consume the manager-owned PCM16 ingress ring through the proven TX
 *        conversion path.
 *
 * The producer never touches I2S: it only wakes this task after a bounded
 * copy. A 0.96 s prefill and 7.68 s bounded PSRAM ring absorb ordinary
 * WebSocket/Opus burst and scheduling jitter without reverting to full-file
 * buffering. A later dry ingress is still reported and bounded, but feeds
 * explicit silence to I2S until it recovers. This prevents a DMA-held audio
 * block from sounding like repeated speech during a short cloud gap.
 */
static esp_err_t play_pcm16_stream(
    uint32_t generation,
    bool *cancelled)
{
    if ((generation == 0U) || (cancelled == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *cancelled = false;
    bool prefill_started = false;
    uint32_t prefill_waited_ms = 0U;
    for (;;)
    {
        if (audio_manager_cancel_is_requested())
        {
            *cancelled = true;
            return ESP_OK;
        }

        audio_manager_pcm_stream_status_t status = {0};
        const esp_err_t status_result = audio_manager_pcm_stream_get_status(
            generation,
            &status);
        if (status_result != ESP_OK)
        {
            return status_result;
        }
        if (status.aborted || !status.active)
        {
            *cancelled = true;
            return ESP_OK;
        }
        if (status.queued_samples >= AUDIO_MANAGER_PCM_STREAM_PREFILL_SAMPLES ||
            (status.ended && (status.queued_samples > 0U)))
        {
            break;
        }
        if (status.ended)
        {
            return ESP_ERR_INVALID_SIZE;
        }
        if ((status.accepted_samples > 0U) && !prefill_started)
        {
            /* TTS_START can precede the first audio packet by several seconds
             * while the server performs a tool call or synthesizes speech. Do
             * not spend the bounded prefill allowance before PCM exists. */
            prefill_started = true;
            prefill_waited_ms = 0U;
        }
        if (prefill_started &&
            (prefill_waited_ms >= AUDIO_MANAGER_PCM_STREAM_PREFILL_WAIT_MS))
        {
            APP_LOGE(TAG, PCM_STREAM_INITIAL_PREFILL_T_8524E6CE,
                     "PCM stream prefill timed out after first PCM generation=%u accepted=%llu queued=%u required=%u",
                     (unsigned)generation,
                     (unsigned long long)status.accepted_samples,
                     (unsigned)status.queued_samples,
                     (unsigned)AUDIO_MANAGER_PCM_STREAM_PREFILL_SAMPLES);
            return ESP_ERR_TIMEOUT;
        }
        (void)ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(AUDIO_MANAGER_PCM_STREAM_INITIAL_WAIT_POLL_MS));
        if (prefill_started)
        {
            prefill_waited_ms += AUDIO_MANAGER_PCM_STREAM_INITIAL_WAIT_POLL_MS;
        }
    }

    APP_LOGI(TAG, PCM_STREAM_START_GENERATION_55FAC83A,
             "PCM_STREAM START generation=%u prefill=%u samples ring=%u samples",
             (unsigned)generation,
             (unsigned)AUDIO_MANAGER_PCM_STREAM_PREFILL_SAMPLES,
             (unsigned)AUDIO_MANAGER_PCM_STREAM_RING_SAMPLES);

    esp_err_t result = start_i2s_tx();
    bool tx_started = (result == ESP_OK);
    if (tx_started && audio_manager_take_pcm_stream_mutex("marking PCM TX start"))
    {
        s_runtime.pcm_stream_tx_started = true;
        xSemaphoreGive(s_runtime.pcm_stream_mutex);
    }

    if ((result == ESP_OK) && !*cancelled)
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_PCM_STREAM_PRE_PLAYBACK_SILENCE_BLOCKS,
            cancelled);
    }
    if ((result == ESP_OK) && !*cancelled)
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_PLAYBACK);
        audio_manager_set_playback_applied();
    }

    uint32_t starvation_waited_ms = 0U;
    bool starvation_active = false;
    while ((result == ESP_OK) && !*cancelled)
    {
        if (audio_manager_cancel_is_requested())
        {
            *cancelled = true;
            break;
        }

        size_t samples_read = 0U;
        bool ended = false;
        bool aborted = false;
        bool active = false;
        if (!audio_manager_take_pcm_stream_mutex("reading PCM stream"))
        {
            result = ESP_ERR_TIMEOUT;
            break;
        }

        result = audio_manager_pcm_stream_core_read(
            &s_runtime.pcm_stream,
            generation,
            s_pcm_stream_mono_block,
            AUDIO_MANAGER_FRAMES_PER_BLOCK,
            &samples_read);
        ended = s_runtime.pcm_stream.ended;
        aborted = s_runtime.pcm_stream.aborted;
        active = s_runtime.pcm_stream.active;
        if ((result == ESP_ERR_NOT_FOUND) && active && !ended && !aborted &&
            !starvation_active)
        {
            ++s_runtime.pcm_stream.starvation_count;
            starvation_active = true;
        }
        xSemaphoreGive(s_runtime.pcm_stream_mutex);

        if (result == ESP_ERR_NOT_FOUND)
        {
            if (aborted || !active)
            {
                *cancelled = true;
                result = ESP_OK;
                break;
            }
            if (ended)
            {
                result = ESP_OK;
                break;
            }
            result = write_silence_blocks(1U, cancelled);
            if ((result != ESP_OK) || *cancelled)
            {
                break;
            }

            starvation_waited_ms += AUDIO_MANAGER_PCM_STREAM_SILENCE_BLOCK_MS;
            if (starvation_waited_ms >=
                AUDIO_MANAGER_PCM_STREAM_STARVATION_WAIT_MS)
            {
                APP_LOGE(TAG, PCM_STREAM_STARVED_GENERATIO_8BE8D1BD,
                         "PCM stream starved generation=%u waited=%ums",
                         (unsigned)generation,
                         (unsigned)starvation_waited_ms);
                result = ESP_ERR_TIMEOUT;
                break;
            }
            result = ESP_OK;
            continue;
        }

        if (result != ESP_OK)
        {
            if (aborted || !active)
            {
                *cancelled = true;
                result = ESP_OK;
            }
            break;
        }

        if (starvation_active)
        {
            APP_LOGW(TAG, PCM_STREAM_INGRESS_RESUMED_G_7C6FB360,
                     "PCM stream ingress resumed generation=%u silence=%ums",
                     (unsigned)generation,
                     (unsigned)starvation_waited_ms);
            starvation_active = false;
        }
        starvation_waited_ms = 0U;
        for (size_t frame = 0U; frame < samples_read; ++frame)
        {
            const int16_t fixed_scaled_sample =
                audio_wav_pcm16_scale_full_range_to_peak(
                    s_pcm_stream_mono_block[frame],
                    AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
            const int16_t mono_sample = apply_wav_volume_percent(
                fixed_scaled_sample);
            const size_t slot = frame * AUDIO_MANAGER_SLOT_COUNT;
            s_tx_block[slot] = mono_sample;
            s_tx_block[slot + 1U] = mono_sample;
        }

        result = write_tx_frames(samples_read);
    }

    if ((result == ESP_OK) && !*cancelled)
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_PCM_STREAM_PRE_PLAYBACK_SILENCE_BLOCKS,
            cancelled);
    }

    if (tx_started)
    {
        const esp_err_t stop_result = stop_i2s_tx();
        if ((result == ESP_OK) && (stop_result != ESP_OK))
        {
            result = stop_result;
        }
    }

    if (audio_manager_take_pcm_stream_mutex("marking PCM TX stop"))
    {
        s_runtime.pcm_stream_tx_started = false;
        xSemaphoreGive(s_runtime.pcm_stream_mutex);
    }
    return result;
}

/**
 * @brief Join the reader, then copy its final metrics before source teardown.
 *
 * Retrying a bounded join is deliberate: freeing PSRAM or closing a FILE
 * while a VFS read is still running would be unsafe.  The public stop call
 * retains its own finite timeout and reports that a wedged VFS read is still
 * draining.
 */
static esp_err_t audio_manager_copy_prefetch_metrics(
    audio_wav_prefetch_t *prefetch,
    audio_wav_playback_metrics_t *metrics)
{
    if ((prefetch == NULL) || (metrics == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    audio_wav_prefetch_request_stop(prefetch);
    uint32_t wait_rounds = 0U;
    while (audio_wav_prefetch_wait_stopped(
               prefetch,
               pdMS_TO_TICKS(AUDIO_MANAGER_WAV_PREFETCH_WAIT_POLL_MS)) ==
           ESP_ERR_TIMEOUT)
    {
        if ((wait_rounds == 0U) || ((wait_rounds % 10U) == 9U))
        {
            APP_LOGW(TAG, WAITING_FOR_WAV_PREFETCH_REA_1A36C5F7, "Waiting for WAV prefetch reader to drain");
        }
        ++wait_rounds;
    }

    audio_wav_prefetch_metrics_t prefetch_metrics = {0};
    const esp_err_t metrics_result = audio_wav_prefetch_get_metrics(
        prefetch,
        &prefetch_metrics);
    if (metrics_result != ESP_OK)
    {
        return metrics_result;
    }

    metrics->data_bytes_read = prefetch_metrics.data_bytes_read;
    metrics->read_count = prefetch_metrics.io_read_count;
    metrics->read_failure_count = prefetch_metrics.io_read_failure_count;
    metrics->max_wav_read_duration_us =
        prefetch_metrics.max_io_read_duration_us;
    metrics->prefetch_blocks_filled = prefetch_metrics.blocks_filled;
    metrics->prefetch_fill_failure_count =
        prefetch_metrics.fill_failure_count;
    metrics->max_prefetch_fill_duration_us =
        prefetch_metrics.max_fill_duration_us;
    metrics->prefetch_recovery_data_offset =
        prefetch_metrics.last_recovery_data_offset;
    metrics->prefetch_recovery_attempt_count =
        prefetch_metrics.recovery_attempt_count;
    metrics->prefetch_recovery_success_count =
        prefetch_metrics.recovery_success_count;
    metrics->prefetch_recovery_wait_ms =
        prefetch_metrics.recovery_wait_ms;
    metrics->prefetch_task_stack_high_water =
        prefetch_metrics.task_stack_high_water;

    return audio_wav_prefetch_get_worker_result(prefetch);
}

/* Static Functions: DSP / Pipeline ---------------------------------------- */
static void dsp_cooperative_yield(void *context)
{
    dsp_scheduler_stats_t *const stats = (dsp_scheduler_stats_t *)context;
    if (stats != NULL)
    {
        ++stats->total_yields;
    }

    vTaskDelay(1U);
}

static void log_ns_metrics(const audio_dsp_ns_metrics_t *metrics)
{
    if (metrics == NULL)
    {
        return;
    }

    const uint32_t floor_per_mille =
        (metrics->processed_bin_frames == 0U)
            ? 0U
            : (uint32_t)(((uint64_t)metrics->floor_bin_frames * 1000U) /
                         metrics->processed_bin_frames);
    const uint32_t protected_per_mille =
        (metrics->processed_bin_frames == 0U)
            ? 0U
            : (uint32_t)(((uint64_t)metrics->speech_protected_bin_frames * 1000U) /
                         metrics->processed_bin_frames);
    const uint32_t average_gain_milli =
        (uint32_t)(metrics->average_gain * 1000.0f + 0.5f);
    const uint32_t minimum_gain_milli =
        (uint32_t)(metrics->minimum_gain * 1000.0f + 0.5f);
    const uint32_t maximum_gain_milli =
        (uint32_t)(metrics->maximum_gain * 1000.0f + 0.5f);

    APP_LOGI(
        TAG, NS_FRAMES_U_UPDATES_U_60F68A0C,
        "NS frames=%u updates=%u yields=%u avg_gain=%u.%03u min=%u.%03u max=%u.%03u floor=%u.%u%% protected=%u.%u%%",
        (unsigned)metrics->processed_frames,
        (unsigned)metrics->noise_updates,
        (unsigned)metrics->cooperative_yields,
        (unsigned)(average_gain_milli / 1000U),
        (unsigned)(average_gain_milli % 1000U),
        (unsigned)(minimum_gain_milli / 1000U),
        (unsigned)(minimum_gain_milli % 1000U),
        (unsigned)(maximum_gain_milli / 1000U),
        (unsigned)(maximum_gain_milli % 1000U),
        (unsigned)(floor_per_mille / 10U),
        (unsigned)(floor_per_mille % 10U),
        (unsigned)(protected_per_mille / 10U),
        (unsigned)(protected_per_mille % 10U));
}

static void log_playback_result(const audio_dsp_playback_stats_t *playback)
{
    if (playback == NULL)
    {
        return;
    }

    const uint32_t output_average =
        (playback->sample_count == 0U)
            ? 0U
            : (uint32_t)(playback->absolute_sum / playback->sample_count);

    APP_LOGI(
        TAG, PLAYBACK_OUTPUT_AVG_U_PEAK_A035815C,
        "PLAYBACK output_avg=%u peak=%u soft=%u/%u limited=%u/%u",
        (unsigned)output_average,
        (unsigned)playback->peak,
        (unsigned)playback->soft_compressed_samples,
        (unsigned)playback->sample_count,
        (unsigned)playback->limited_samples,
        (unsigned)playback->sample_count);
}

static esp_err_t record_once_controlled(
    size_t target_sample_count,
    audio_record_control_t control,
    size_t *samples_recorded,
    audio_record_stop_reason_t *stop_reason)
{
    if ((samples_recorded == NULL) ||
        (stop_reason == NULL) ||
        (target_sample_count == 0U) ||
        (target_sample_count > s_runtime.sample_capacity))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_recorded = 0U;
    *stop_reason = AUDIO_RECORD_STOP_NONE;
    microphone_slot_t selected_slot = MICROPHONE_SLOT_LEFT;

    esp_err_t result = hold_amplifier_data_low();
    if (result != ESP_OK)
    {
        return result;
    }

    result = start_i2s_rx();
    if (result != ESP_OK)
    {
        return result;
    }

    /* RX is active during startup discard and slot detection too. */
    audio_manager_set_state(AUDIO_MANAGER_STATE_RECORDING);

    result = discard_microphone_startup(control, stop_reason);
    if ((result == ESP_OK) && (*stop_reason == AUDIO_RECORD_STOP_NONE))
    {
        result = detect_microphone_slot(
            &selected_slot,
            control,
            stop_reason);
    }

    if ((result == ESP_OK) && (*stop_reason == AUDIO_RECORD_STOP_NONE))
    {
        APP_LOGI(
            TAG, RECORDING_TARGET_U_SAMPLES_U_4EE0A79C,
            "RECORDING target=%u samples (%u seconds) mode=%s",
            (unsigned)target_sample_count,
            (unsigned)(target_sample_count / AUDIO_MANAGER_SAMPLE_RATE_HZ),
            (control == AUDIO_RECORD_CONTROL_MANUAL) ? "manual" : "fixed");
        result = record_audio(
            selected_slot,
            target_sample_count,
            control,
            samples_recorded,
            stop_reason);
    }

    const esp_err_t stop_result = stop_i2s_rx();
    if ((result == ESP_OK) && (stop_result != ESP_OK))
    {
        result = stop_result;
    }

    if (control == AUDIO_RECORD_CONTROL_GOLDEN_FIXED)
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    }

    return result;
}

static esp_err_t record_once(size_t *samples_recorded)
{
    audio_record_stop_reason_t stop_reason = AUDIO_RECORD_STOP_NONE;
    return record_once_controlled(
        s_runtime.fixed_record_sample_count,
        AUDIO_RECORD_CONTROL_GOLDEN_FIXED,
        samples_recorded,
        &stop_reason);
}

static esp_err_t process_once(
    size_t sample_count,
    audio_cycle_metrics_t *metrics)
{
    if ((sample_count < AUDIO_DSP_NS_FFT_SIZE) ||
        (sample_count > s_runtime.sample_capacity) ||
        (metrics == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    audio_manager_set_state(AUDIO_MANAGER_STATE_PROCESSING);
    memset(metrics, 0, sizeof(*metrics));
    metrics->samples_recorded = sample_count;

    dsp_scheduler_stats_t scheduler = {0};

    audio_dsp_calculate_level_cooperative(
        s_runtime.recording_pcm24,
        sample_count,
        &metrics->raw_average_pcm24,
        &metrics->raw_peak_pcm24,
        dsp_cooperative_yield,
        &scheduler);

    const int64_t dsp_start_us = esp_timer_get_time();

    metrics->dc_offset_pcm24 = audio_dsp_calculate_dc_offset_cooperative(
        s_runtime.recording_pcm24,
        sample_count,
        dsp_cooperative_yield,
        &scheduler);

    audio_dsp_apply_speech_band_filter_in_place_cooperative(
        s_runtime.recording_pcm24,
        sample_count,
        metrics->dc_offset_pcm24,
        dsp_cooperative_yield,
        &scheduler);

    audio_dsp_calculate_level_cooperative(
        s_runtime.recording_pcm24,
        sample_count,
        &metrics->band_average_pcm24,
        &metrics->band_peak_pcm24,
        dsp_cooperative_yield,
        &scheduler);

    if (!audio_dsp_apply_adaptive_ns_in_place_cooperative(
            s_runtime.recording_pcm24,
            sample_count,
            s_runtime.dsp_workspace,
            &metrics->ns,
            dsp_cooperative_yield,
            &scheduler))
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        return ESP_FAIL;
    }

    const int64_t elapsed_us = esp_timer_get_time() - dsp_start_us;

    audio_dsp_calculate_level_cooperative(
        s_runtime.recording_pcm24,
        sample_count,
        &metrics->ns_average_pcm24,
        &metrics->ns_peak_pcm24,
        dsp_cooperative_yield,
        &scheduler);

    metrics->dsp_processing_ms = (uint32_t)(elapsed_us / 1000LL);
    metrics->dsp_total_yields = scheduler.total_yields;

    const uint64_t audio_duration_us =
        ((uint64_t)sample_count * 1000000ULL) /
        AUDIO_MANAGER_SAMPLE_RATE_HZ;
    metrics->dsp_realtime_factor_milli =
        (audio_duration_us == 0U)
            ? 0U
            : (uint32_t)(((uint64_t)elapsed_us * 1000ULL) /
                         audio_duration_us);

    APP_LOGI(
        TAG, DSP_DC_LD_PCM16_EQ_AAC46990,
        "DSP dc=%ld pcm16_eq raw_avg=%u raw_peak=%u band_avg=%u band_peak=%u ns_avg=%u ns_peak=%u time=%ums rt=%u.%03u",
        (long)metrics->dc_offset_pcm24,
        (unsigned)(metrics->raw_average_pcm24 / AUDIO_DSP_PCM24_SCALE_FACTOR),
        (unsigned)(metrics->raw_peak_pcm24 / AUDIO_DSP_PCM24_SCALE_FACTOR),
        (unsigned)(metrics->band_average_pcm24 / AUDIO_DSP_PCM24_SCALE_FACTOR),
        (unsigned)(metrics->band_peak_pcm24 / AUDIO_DSP_PCM24_SCALE_FACTOR),
        (unsigned)(metrics->ns_average_pcm24 / AUDIO_DSP_PCM24_SCALE_FACTOR),
        (unsigned)(metrics->ns_peak_pcm24 / AUDIO_DSP_PCM24_SCALE_FACTOR),
        (unsigned)metrics->dsp_processing_ms,
        (unsigned)(metrics->dsp_realtime_factor_milli / 1000U),
        (unsigned)(metrics->dsp_realtime_factor_milli % 1000U));

    log_ns_metrics(&metrics->ns);
    audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    return ESP_OK;
}

static esp_err_t playback_once(
    audio_playback_source_t *source,
    audio_cycle_metrics_t *metrics,
    bool *cancelled)
{
    if ((source == NULL) || (metrics == NULL) || (cancelled == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *cancelled = false;

    switch (source->kind)
    {
        case AUDIO_PLAYBACK_SOURCE_RECORDED_PCM24:
            if ((source->recorded_sample_count == 0U) ||
                (source->recorded_sample_count > s_runtime.sample_capacity))
            {
                return ESP_ERR_INVALID_ARG;
            }
            break;

        case AUDIO_PLAYBACK_SOURCE_WAV_PCM16:
            if (!audio_wav_prefetch_is_active(&source->wav_prefetch))
            {
                return ESP_ERR_INVALID_STATE;
            }
            APP_LOGI(
                TAG, WAV_PREFETCH_PLAYBACK_BLOCK_26BE1A17,
                "WAV prefetch playback block=%uB cache=%us x%u volume=%u/100 policy=fixed_full_scale_pcm16 fixed_gain_q16=%u ceiling=+/-%u",
                (unsigned)AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES,
                (unsigned)CONFIG_AUDIO_MANAGER_WAV_PREFETCH_SECONDS,
                (unsigned)AUDIO_WAV_PREFETCH_SLOT_COUNT,
                (unsigned)s_runtime.config.playback_volume_percent,
                (unsigned)AUDIO_WAV_PCM16_FULL_SCALE_GAIN_Q16(
                    AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16),
                (unsigned)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
            audio_playback_flow_t wav_flow = AUDIO_PLAYBACK_FLOW_CONTINUE;
            uint64_t next_data_offset = 0U;
            audio_wav_info_t info = {0};
            bool info_valid = false;
            const esp_err_t wav_result = play_wav_stream(
                &source->wav_prefetch,
                0U,
                &metrics->wav,
                &wav_flow,
                &next_data_offset,
                &info,
                &info_valid);
            (void)next_data_offset;
            (void)info;
            (void)info_valid;
            *cancelled = (wav_flow == AUDIO_PLAYBACK_FLOW_CANCEL);
            return wav_result;

        case AUDIO_PLAYBACK_SOURCE_PCM16_STREAM:
            if (source->stream_generation == 0U)
            {
                return ESP_ERR_INVALID_ARG;
            }
            return play_pcm16_stream(source->stream_generation, cancelled);

        default:
            return ESP_ERR_INVALID_ARG;
    }

    const bool cancellable = audio_manager_recorded_playback_cancel_enabled();

    esp_err_t result = start_i2s_tx();
    if (result != ESP_OK)
    {
        return result;
    }

    audio_playback_flow_t flow = AUDIO_PLAYBACK_FLOW_CONTINUE;
    if (cancellable)
    {
        result = write_controlled_silence_blocks(
            AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS,
            &flow);
    }
    else
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS,
            NULL);
    }

    if ((result == ESP_OK) &&
        (flow == AUDIO_PLAYBACK_FLOW_CONTINUE))
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_PLAYBACK);

        APP_LOGI(
            TAG, PLAYBACK_SAMPLES_U_VOLUME_U_9D833C5E,
            "PLAYBACK samples=%u volume=%u/100 gain=%u.%02ux limiter=+/-%u",
            (unsigned)source->recorded_sample_count,
            (unsigned)s_runtime.config.playback_volume_percent,
            (unsigned)(AUDIO_DSP_PLAYBACK_GAIN_Q8 / AUDIO_DSP_Q8_ONE),
            (unsigned)(((AUDIO_DSP_PLAYBACK_GAIN_Q8 %
                          AUDIO_DSP_Q8_ONE) * 100U) /
                        AUDIO_DSP_Q8_ONE),
            (unsigned)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
        result = play_recording(
            0U,
            source->recorded_sample_count,
            &metrics->playback,
            &flow,
            &(size_t){0U});
    }

    *cancelled = (flow == AUDIO_PLAYBACK_FLOW_CANCEL);

    if ((result == ESP_OK) && !*cancelled)
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_POST_PLAYBACK_SILENCE_BLOCKS,
            cancellable ? cancelled : NULL);
    }

    const esp_err_t stop_result = stop_i2s_tx();
    if ((result == ESP_OK) && (stop_result != ESP_OK))
    {
        result = stop_result;
    }

    if ((result == ESP_OK) &&
        !*cancelled &&
        (source->kind == AUDIO_PLAYBACK_SOURCE_RECORDED_PCM24))
    {
        log_playback_result(&metrics->playback);
    }

    return result;
}

static esp_err_t audio_manager_select_recording_playback_source(
    size_t sample_count)
{
    if ((sample_count == 0U) ||
        (sample_count > s_runtime.sample_capacity))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime.playback_source.kind != AUDIO_PLAYBACK_SOURCE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.playback_source.kind = AUDIO_PLAYBACK_SOURCE_RECORDED_PCM24;
    s_runtime.playback_source.recorded_sample_count = sample_count;
    return ESP_OK;
}

static esp_err_t audio_manager_select_wav_playback_source(
    const char *path,
    uint64_t committed_data_offset,
    const audio_wav_info_t *expected_info)
{
    if ((path == NULL) || (path[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime.playback_source.kind != AUDIO_PLAYBACK_SOURCE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_wav_prefetch_is_active(
            &s_runtime.playback_source.wav_prefetch))
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = audio_wav_prefetch_start_at_offset(
        &s_runtime.playback_source.wav_prefetch,
        path,
        AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES,
        AUDIO_MANAGER_WAV_PREFETCH_READER_PRIORITY,
        committed_data_offset,
        expected_info);
    if (result != ESP_OK)
    {
        return result;
    }

    s_runtime.playback_source.kind = AUDIO_PLAYBACK_SOURCE_WAV_PCM16;
    return ESP_OK;
}

static esp_err_t audio_manager_select_pcm_stream_playback_source(
    uint32_t generation)
{
    if (generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime.playback_source.kind != AUDIO_PLAYBACK_SOURCE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_manager_pcm_stream_status_t status = {0};
    const esp_err_t result = audio_manager_pcm_stream_get_status(
        generation,
        &status);
    if ((result != ESP_OK) || !status.active || status.aborted)
    {
        return (result == ESP_OK) ? ESP_ERR_INVALID_STATE : result;
    }

    s_runtime.playback_source.kind = AUDIO_PLAYBACK_SOURCE_PCM16_STREAM;
    s_runtime.playback_source.stream_generation = generation;
    return ESP_OK;
}

static esp_err_t audio_manager_release_playback_source(void)
{
    esp_err_t result = ESP_OK;

    if ((s_runtime.playback_source.kind == AUDIO_PLAYBACK_SOURCE_WAV_PCM16) ||
        audio_wav_prefetch_is_active(
            &s_runtime.playback_source.wav_prefetch))
    {
        uint32_t wait_rounds = 0U;
        do
        {
            result = audio_wav_prefetch_stop_and_destroy(
                &s_runtime.playback_source.wav_prefetch,
                pdMS_TO_TICKS(AUDIO_MANAGER_WAV_PREFETCH_WAIT_POLL_MS));
            if (result == ESP_ERR_TIMEOUT)
            {
                if ((wait_rounds == 0U) || ((wait_rounds % 10U) == 9U))
                {
                    APP_LOGW(TAG, WAITING_TO_RELEASE_WAV_PREFE_9AF2F66B, "Waiting to release WAV prefetch reader");
                }
                ++wait_rounds;
            }
        } while (result == ESP_ERR_TIMEOUT);

        if (audio_wav_prefetch_is_active(
                &s_runtime.playback_source.wav_prefetch))
        {
            return result;
        }
    }

    s_runtime.playback_source.kind = AUDIO_PLAYBACK_SOURCE_NONE;
    s_runtime.playback_source.recorded_sample_count = 0U;
    s_runtime.playback_source.stream_generation = 0U;
    return result;
}

static esp_err_t force_cycle_cleanup(void)
{
    esp_err_t first_error = ESP_OK;

    const esp_err_t rx_result = stop_i2s_rx();
    if ((first_error == ESP_OK) && (rx_result != ESP_OK))
    {
        first_error = rx_result;
    }

    const esp_err_t tx_result = stop_i2s_tx();
    if ((first_error == ESP_OK) && (tx_result != ESP_OK))
    {
        first_error = tx_result;
    }

    const esp_err_t source_result = audio_manager_release_playback_source();
    if ((first_error == ESP_OK) && (source_result != ESP_OK))
    {
        first_error = source_result;
    }

    const esp_err_t gpio_result = hold_amplifier_data_low();
    if ((first_error == ESP_OK) && (gpio_result != ESP_OK))
    {
        first_error = gpio_result;
    }

    return first_error;
}

static esp_err_t run_cycle(audio_cycle_metrics_t *metrics)
{
    if (metrics == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(metrics, 0, sizeof(*metrics));

    size_t samples_recorded = 0U;
    esp_err_t result = record_once(&samples_recorded);

    if ((result == ESP_OK) &&
        (samples_recorded != s_runtime.fixed_record_sample_count))
    {
        APP_LOGE(
            TAG, RECORDING_INCOMPLETE_GOT_U_E_FB3425AD,
            "Recording incomplete: got=%u expected=%u",
            (unsigned)samples_recorded,
            (unsigned)s_runtime.fixed_record_sample_count);
        result = ESP_FAIL;
    }

    if (result == ESP_OK)
    {
        result = process_once(samples_recorded, metrics);
    }

    if (result == ESP_OK)
    {
        result = audio_manager_select_recording_playback_source(
            samples_recorded);
    }

    if (result == ESP_OK)
    {
        bool cancelled = false;
        vTaskDelay(pdMS_TO_TICKS(AUDIO_MANAGER_PRE_PLAYBACK_DELAY_MS));
        result = playback_once(
            &s_runtime.playback_source,
            metrics,
            &cancelled);
    }

    metrics->samples_recorded = samples_recorded;

    const esp_err_t cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (cleanup_result != ESP_OK))
    {
        result = cleanup_result;
    }

    return result;
}

static bool audio_manager_stability_mode_enabled(void)
{
    return false;
}

static bool audio_manager_mixed_stress_mode_enabled(void)
{
    return false;
}

static esp_err_t audio_manager_queue_simple_operation(
    audio_manager_command_kind_t command_kind,
    audio_manager_operation_t operation,
    bool require_recorded_audio,
    const char *description)
{
    if (!s_runtime.initialized ||
        (s_runtime.status_mutex == NULL) ||
        (s_runtime.command_queue == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Golden mode owns the record buffer/I2S cycle; production commands stay out. */
    if (audio_manager_stability_mode_enabled())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_status_mutex(description))
    {
        return ESP_ERR_TIMEOUT;
    }

    if (require_recorded_audio &&
        (!s_runtime.recorded_audio_valid ||
         (s_runtime.recorded_sample_count == 0U)))
    {
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    bool accepted = false;
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.task_running &&
        !s_control.shutdown_requested &&
        (s_control.operation == AUDIO_MANAGER_OPERATION_NONE) &&
        (s_runtime.status.state == AUDIO_MANAGER_STATE_IDLE) &&
        ((operation != AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK) ||
         (s_control.playback_status.state ==
          AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE)))
    {
        s_control.operation = operation;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
        if (operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK)
        {
            audio_manager_begin_playback_control_locked(
                AUDIO_MANAGER_PLAYBACK_SOURCE_RECORDED,
                true,
                s_runtime.recorded_sample_count);
        }
        accepted = true;
    }
    portEXIT_CRITICAL(&s_control_lock);

    if (!accepted)
    {
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const audio_manager_command_t command = {
        .kind = command_kind,
    };
    if (xQueueSend(s_runtime.command_queue, &command, 0U) != pdTRUE)
    {
        if (operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK)
        {
            audio_manager_abort_playback_control_begin();
        }
        audio_manager_finish_operation();
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_runtime.status_mutex);
    return ESP_OK;
}

static void audio_manager_handle_record_command(bool manual)
{
    APP_LOGI(
        TAG, S_RECORDING_7CC265FE,
        "========== %s RECORDING ==========" ,
        manual ? "MANUAL" : "FIXED");

    if (audio_manager_take_status_mutex("starting recording"))
    {
        ++s_runtime.status.recording_started;
        s_runtime.recorded_audio_valid = false;
        s_runtime.recorded_sample_count = 0U;
        s_runtime.status.recorded_audio_available = false;
        xSemaphoreGive(s_runtime.status_mutex);
    }
    else
    {
        s_runtime.recorded_audio_valid = false;
        s_runtime.recorded_sample_count = 0U;
    }

    const size_t target_sample_count =
        manual
            ? s_runtime.manual_record_sample_limit
            : s_runtime.fixed_record_sample_count;
    const audio_record_control_t control =
        manual
            ? AUDIO_RECORD_CONTROL_MANUAL
            : AUDIO_RECORD_CONTROL_PRODUCTION_FIXED;

    size_t samples_recorded = 0U;
    audio_record_stop_reason_t stop_reason = AUDIO_RECORD_STOP_NONE;
    audio_cycle_metrics_t metrics = {0};
    esp_err_t result = record_once_controlled(
        target_sample_count,
        control,
        &samples_recorded,
        &stop_reason);

    const bool aborted = (stop_reason == AUDIO_RECORD_STOP_ABORT);
    const bool manual_stopped = (stop_reason == AUDIO_RECORD_STOP_MANUAL);
    bool processed = false;

    if ((result == ESP_OK) && !aborted)
    {
        if (samples_recorded >= AUDIO_DSP_NS_FFT_SIZE)
        {
            result = process_once(samples_recorded, &metrics);
            processed = (result == ESP_OK);
        }
        else if (manual && manual_stopped)
        {
            APP_LOGW(
                TAG, MANUAL_RECORDING_STOPPED_BEF_C4AF29C4,
                "Manual recording stopped before DSP minimum: samples=%u minimum=%u; discarded",
                (unsigned)samples_recorded,
                (unsigned)AUDIO_DSP_NS_FFT_SIZE);
        }
        else
        {
            result = ESP_ERR_INVALID_SIZE;
        }
    }

    if (processed)
    {
        s_runtime.recorded_audio_valid = true;
        s_runtime.recorded_sample_count = samples_recorded;
    }

    const esp_err_t cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (cleanup_result != ESP_OK))
    {
        result = cleanup_result;
        s_runtime.recorded_audio_valid = false;
        s_runtime.recorded_sample_count = 0U;
        processed = false;
    }
    else if ((result != ESP_OK) && (cleanup_result != ESP_OK))
    {
        APP_LOGE(
            TAG, RECORDING_CLEANUP_ALSO_FAILE_1D0278F6,
            "Recording cleanup also failed: %s",
            esp_err_to_name(cleanup_result));
    }

    bool status_updated = false;
    if (audio_manager_take_status_mutex("storing recording result"))
    {
        s_runtime.status.last_samples_recorded = samples_recorded;
        s_runtime.status.recorded_audio_available = s_runtime.recorded_audio_valid;

        if (aborted)
        {
            s_runtime.status.last_error = ESP_OK;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }
        else if (result != ESP_OK)
        {
            s_runtime.status.last_error = result;
            ++s_runtime.status.recording_failed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_ERROR;
        }
        else
        {
            s_runtime.status.last_error = ESP_OK;
            if (manual_stopped)
            {
                ++s_runtime.status.recording_manual_stopped;
            }
            if (processed)
            {
                ++s_runtime.status.recording_completed;
            }
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }

        audio_manager_refresh_diagnostics_locked();
        status_updated = true;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    audio_manager_finish_operation();

    if (status_updated)
    {
        audio_manager_notify_status_changed();
    }

    if (aborted)
    {
        APP_LOGI(
            TAG, S_RECORDING_ABORTED_BY_MANAG_3BB8A036,
            "%s recording aborted by manager shutdown",
            manual ? "Manual" : "Fixed");
    }
    else if (result != ESP_OK)
    {
        APP_LOGE(
            TAG, S_RECORDING_FAILED_S_1B234FDD,
            "%s recording failed: %s",
            manual ? "Manual" : "Fixed",
            esp_err_to_name(result));
        if (status_updated)
        {
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        }
    }
    else if (processed)
    {
        APP_LOGI(
            TAG, S_RECORDING_COMPLETED_SAMPLE_16D3F247,
            "%s recording completed: samples=%u manual_stop=%s",
            manual ? "Manual" : "Fixed",
            (unsigned)samples_recorded,
            manual_stopped ? "yes" : "no");
    }
    else
    {
        APP_LOGI(
            TAG, MANUAL_RECORDING_ENDED_WITHO_230458E1,
            "Manual recording ended without retained audio: samples=%u",
            (unsigned)samples_recorded);
    }
}

static void audio_manager_handle_recorded_playback_command(
    bool retained_resume,
    bool restart_from_beginning)
{
    APP_LOGI(TAG, RECORDED_PLAYBACK_CB11BCBF, "========== RECORDED PLAYBACK ==========");

    if (!retained_resume &&
        audio_manager_take_status_mutex("starting recorded playback"))
    {
        ++s_runtime.status.recorded_playback_started;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    const size_t sample_count = retained_resume
        ? s_runtime.playback_resume.recorded_sample_count
        : s_runtime.recorded_sample_count;
    audio_cycle_metrics_t metrics = {0};
    bool cancelled = audio_manager_cancel_is_requested();
    esp_err_t result = ESP_OK;
    size_t sample_index = (retained_resume && !restart_from_beginning)
        ? (size_t)s_runtime.playback_resume.position_frames
        : 0U;
    bool suspended = false;

    if (!s_runtime.recorded_audio_valid || (sample_count == 0U) ||
        (retained_resume &&
         (!s_runtime.playback_resume.valid ||
          (s_runtime.playback_resume.kind !=
           AUDIO_PLAYBACK_SOURCE_RECORDED_PCM24) ||
          (sample_index > sample_count))))
    {
        result = ESP_ERR_INVALID_STATE;
    }

    if ((result == ESP_OK) && !cancelled)
    {
        result = audio_manager_select_recording_playback_source(sample_count);
    }

    while ((result == ESP_OK) && !cancelled &&
           (sample_index < sample_count))
    {
        audio_playback_flow_t flow = audio_manager_playback_flow_requested();
        if (flow == AUDIO_PLAYBACK_FLOW_CANCEL)
        {
            cancelled = true;
            break;
        }
        if (flow == AUDIO_PLAYBACK_FLOW_RESTART)
        {
            sample_index = 0U;
            audio_manager_update_playback_position(0U);
            portENTER_CRITICAL(&s_control_lock);
            s_control.restart_requested = false;
            portEXIT_CRITICAL(&s_control_lock);
        }

        bool tx_started = false;
        if (flow != AUDIO_PLAYBACK_FLOW_PAUSE)
        {
            result = start_i2s_tx();
            tx_started = (result == ESP_OK);
        }
        if ((result == ESP_OK) && tx_started &&
            (sample_index == 0U))
        {
            result = write_controlled_silence_blocks(
                AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS,
                &flow);
        }
        if ((result == ESP_OK) && tx_started &&
            (flow == AUDIO_PLAYBACK_FLOW_CONTINUE))
        {
            audio_manager_set_state(AUDIO_MANAGER_STATE_PLAYBACK);
            audio_manager_set_playback_applied();
            result = play_recording(
                sample_index,
                sample_count,
                &metrics.playback,
                &flow,
                &sample_index);
        }

        const bool natural_end =
            (result == ESP_OK) &&
            (flow == AUDIO_PLAYBACK_FLOW_CONTINUE) &&
            (sample_index == sample_count);
        if (natural_end)
        {
            bool terminal_cancelled = false;
            result = write_silence_blocks(
                AUDIO_MANAGER_POST_PLAYBACK_SILENCE_BLOCKS,
                &terminal_cancelled);
            if (terminal_cancelled)
            {
                flow = AUDIO_PLAYBACK_FLOW_CANCEL;
            }
        }

        if (tx_started)
        {
            const esp_err_t stop_result = stop_i2s_tx();
            if ((result == ESP_OK) && (stop_result != ESP_OK))
            {
                result = stop_result;
            }
        }
        if (result == ESP_OK)
        {
            const audio_playback_flow_t late_flow =
                audio_manager_playback_flow_requested();
            if ((late_flow == AUDIO_PLAYBACK_FLOW_CANCEL) ||
                (late_flow == AUDIO_PLAYBACK_FLOW_RESTART) ||
                ((late_flow == AUDIO_PLAYBACK_FLOW_PAUSE) &&
                 (sample_index < sample_count)))
            {
                flow = late_flow;
            }
        }
        if (result != ESP_OK)
        {
            break;
        }

        if (flow == AUDIO_PLAYBACK_FLOW_CANCEL)
        {
            cancelled = true;
            break;
        }
        if (flow == AUDIO_PLAYBACK_FLOW_RESTART)
        {
            sample_index = 0U;
            audio_manager_update_playback_position(0U);
            portENTER_CRITICAL(&s_control_lock);
            s_control.restart_requested = false;
            portEXIT_CRITICAL(&s_control_lock);
            continue;
        }
        if (flow == AUDIO_PLAYBACK_FLOW_PAUSE)
        {
            audio_manager_playback_pause_reason_t pause_reason =
                AUDIO_MANAGER_PLAYBACK_PAUSE_USER;
            uint32_t generation = 0U;
            audio_manager_get_pause_metadata(&pause_reason, &generation);
            s_runtime.playback_resume = (audio_playback_resume_context_t) {
                .valid = true,
                .kind = AUDIO_PLAYBACK_SOURCE_RECORDED_PCM24,
                .generation = generation,
                .pause_reason = pause_reason,
                .position_frames = sample_index,
                .total_frames = sample_count,
                .recorded_sample_count = sample_count,
            };
            audio_manager_set_playback_paused(pause_reason, sample_index);
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
            APP_LOGI(
                TAG, RECORDED_PLAYBACK_PAUSED_AT_8CB6E36E,
                "Recorded playback paused generation=%u frame=%llu",
                (unsigned)generation,
                (unsigned long long)sample_index);

            suspended = true;
            break;
        }
    }

    if ((result == ESP_OK) && !cancelled &&
        (sample_index == sample_count))
    {
        log_playback_result(&metrics.playback);
    }

    const esp_err_t cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (cleanup_result != ESP_OK))
    {
        result = cleanup_result;
        cancelled = false;
    }
    else if ((result != ESP_OK) && (cleanup_result != ESP_OK))
    {
        APP_LOGE(
            TAG, RECORDED_PLAYBACK_CLEANUP_AL_93088591,
            "Recorded playback cleanup also failed: %s",
            esp_err_to_name(cleanup_result));
    }

    if (suspended && (result == ESP_OK))
    {
        audio_manager_suspend_operation();
        audio_manager_notify_status_changed();
        if (audio_manager_consume_resume_requested())
        {
            const esp_err_t resume_result =
                audio_manager_queue_retained_playback(0U, false);
            if (resume_result != ESP_OK)
            {
                APP_LOGW(TAG, RAPID_RESUME_QUEUE_FAILED_6954B731,
                         "Recorded rapid resume queue failed: %s",
                         esp_err_to_name(resume_result));
            }
        }
        return;
    }

    audio_manager_complete_playback_control(result);
    audio_manager_clear_resume_context();

    bool status_updated = false;
    if (audio_manager_take_status_mutex("storing recorded playback result"))
    {
        s_runtime.status.last_error = result;
        if (result != ESP_OK)
        {
            ++s_runtime.status.recorded_playback_failed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_ERROR;
        }
        else if (cancelled)
        {
            ++s_runtime.status.recorded_playback_cancelled;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }
        else
        {
            ++s_runtime.status.recorded_playback_completed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }

        audio_manager_refresh_diagnostics_locked();
        status_updated = true;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    audio_manager_finish_operation();

    if (status_updated)
    {
        audio_manager_notify_status_changed();
    }

    if (result != ESP_OK)
    {
        APP_LOGE(
            TAG, RECORDED_PLAYBACK_FAILED_S_C73F775E,
            "Recorded playback failed: %s",
            esp_err_to_name(result));
        if (status_updated)
        {
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        }
    }
    else if (cancelled)
    {
        APP_LOGI(TAG, RECORDED_PLAYBACK_CANCELLED_366AB6AA, "Recorded playback cancelled");
    }
    else
    {
        APP_LOGI(TAG, RECORDED_PLAYBACK_COMPLETED_99A3A041, "Recorded playback completed");
    }
}

static void audio_manager_handle_wav_command(
    const char *path,
    bool retained_resume,
    bool restart_from_beginning)
{
    APP_LOGI(TAG, WAV_PLAYBACK_EF4C2B80, "========== WAV PLAYBACK ==========");
    APP_LOGI(TAG, WAV_PATH_S_53E379BA, "WAV path=%s", path);

    if (!retained_resume &&
        audio_manager_take_status_mutex("starting WAV playback"))
    {
        ++s_runtime.status.wav_playback_started;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    audio_manager_diagnostics_t diagnostics_before = {0};
    audio_manager_snapshot_diagnostics(&diagnostics_before);

    audio_cycle_metrics_t metrics = {0};
    uint32_t expected_data_bytes = 0U;
    uint32_t expected_duration_ms = 0U;
    bool cancelled = audio_manager_cancel_is_requested();
    esp_err_t result = ESP_OK;
    uint64_t data_offset = (retained_resume && !restart_from_beginning)
        ? s_runtime.playback_resume.position_frames * sizeof(int16_t)
        : 0U;
    audio_wav_info_t retained_info = retained_resume
        ? s_runtime.playback_resume.wav_info
        : (audio_wav_info_t) {0};
    bool retained_info_valid =
        retained_resume && s_runtime.playback_resume.wav_info_valid;
    const char *source_path = path;
    bool suspended = false;

    if (retained_resume &&
        (!s_runtime.playback_resume.valid ||
         (s_runtime.playback_resume.kind != AUDIO_PLAYBACK_SOURCE_WAV_PCM16) ||
         !retained_info_valid ||
         (data_offset >= retained_info.data_size_bytes)))
    {
        result = ESP_ERR_INVALID_STATE;
    }

    while ((result == ESP_OK) && !cancelled)
    {
        audio_playback_flow_t flow = audio_manager_playback_flow_requested();
        if (flow == AUDIO_PLAYBACK_FLOW_CANCEL)
        {
            cancelled = true;
            break;
        }
        if (flow == AUDIO_PLAYBACK_FLOW_RESTART)
        {
            data_offset = 0U;
            audio_manager_update_playback_position(0U);
            portENTER_CRITICAL(&s_control_lock);
            s_control.restart_requested = false;
            portEXIT_CRITICAL(&s_control_lock);
        }

        if (flow != AUDIO_PLAYBACK_FLOW_PAUSE)
        {
            result = audio_manager_select_wav_playback_source(
                source_path,
                data_offset,
                retained_info_valid ? &retained_info : NULL);
        }
        if ((result == ESP_OK) && (flow != AUDIO_PLAYBACK_FLOW_PAUSE))
        {
            audio_wav_info_t segment_info = {0};
            bool segment_info_valid = false;
            result = play_wav_stream(
                &s_runtime.playback_source.wav_prefetch,
                data_offset,
                &metrics.wav,
                &flow,
                &data_offset,
                &segment_info,
                &segment_info_valid);
            if (segment_info_valid)
            {
                retained_info = segment_info;
                retained_info_valid = true;
                expected_data_bytes = retained_info.data_size_bytes;
                expected_duration_ms = retained_info.duration_ms;
            }
        }

        if (audio_wav_prefetch_is_active(
                &s_runtime.playback_source.wav_prefetch))
        {
            const esp_err_t prefetch_result =
                audio_manager_copy_prefetch_metrics(
                    &s_runtime.playback_source.wav_prefetch,
                    &metrics.wav);
            if ((result == ESP_OK) &&
                (flow != AUDIO_PLAYBACK_FLOW_CANCEL) &&
                (prefetch_result != ESP_OK))
            {
                result = prefetch_result;
            }
            else if ((result != ESP_OK) &&
                     (prefetch_result != ESP_OK) &&
                     (prefetch_result != result))
            {
                APP_LOGE(
                    TAG, WAV_PREFETCH_READER_ALSO_FAI_FD2BAD4F,
                    "WAV prefetch reader also failed: %s",
                    esp_err_to_name(prefetch_result));
            }
        }

        const esp_err_t cleanup_result = force_cycle_cleanup();
        if ((result == ESP_OK) && (cleanup_result != ESP_OK))
        {
            result = cleanup_result;
            flow = AUDIO_PLAYBACK_FLOW_CONTINUE;
        }
        else if ((result != ESP_OK) && (cleanup_result != ESP_OK))
        {
            APP_LOGE(
                TAG, WAV_CLEANUP_ALSO_FAILED_S_1E2F345D,
                "WAV cleanup also failed: %s",
                esp_err_to_name(cleanup_result));
        }
        if (result == ESP_OK)
        {
            const audio_playback_flow_t late_flow =
                audio_manager_playback_flow_requested();
            if ((late_flow == AUDIO_PLAYBACK_FLOW_CANCEL) ||
                (late_flow == AUDIO_PLAYBACK_FLOW_RESTART) ||
                ((late_flow == AUDIO_PLAYBACK_FLOW_PAUSE) &&
                 (!retained_info_valid ||
                  (data_offset < retained_info.data_size_bytes))))
            {
                flow = late_flow;
            }
        }
        if (result != ESP_OK)
        {
            break;
        }

        if (flow == AUDIO_PLAYBACK_FLOW_CANCEL)
        {
            cancelled = true;
            break;
        }
        if (flow == AUDIO_PLAYBACK_FLOW_RESTART)
        {
            data_offset = 0U;
            audio_manager_update_playback_position(0U);
            portENTER_CRITICAL(&s_control_lock);
            s_control.restart_requested = false;
            portEXIT_CRITICAL(&s_control_lock);
            continue;
        }
        if (flow == AUDIO_PLAYBACK_FLOW_PAUSE)
        {
            audio_manager_playback_pause_reason_t pause_reason =
                AUDIO_MANAGER_PLAYBACK_PAUSE_USER;
            uint32_t generation = 0U;
            audio_manager_get_pause_metadata(&pause_reason, &generation);
            s_runtime.playback_resume.valid = true;
            s_runtime.playback_resume.kind = AUDIO_PLAYBACK_SOURCE_WAV_PCM16;
            s_runtime.playback_resume.generation = generation;
            s_runtime.playback_resume.pause_reason = pause_reason;
            s_runtime.playback_resume.position_frames =
                data_offset / sizeof(int16_t);
            s_runtime.playback_resume.total_frames = retained_info_valid
                ? retained_info.data_size_bytes / sizeof(int16_t)
                : 0U;
            s_runtime.playback_resume.wav_info = retained_info;
            s_runtime.playback_resume.wav_info_valid = retained_info_valid;
            if (source_path != s_runtime.playback_resume.wav_path)
            {
                memcpy(
                    s_runtime.playback_resume.wav_path,
                    source_path,
                    strnlen(source_path,
                            AUDIO_MANAGER_WAV_PATH_MAX_BYTES) + 1U);
            }

            /* Resource teardown above is complete before PAUSED is visible. */
            audio_manager_set_playback_paused(
                pause_reason,
                data_offset / sizeof(int16_t));
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
            APP_LOGI(
                TAG, WAV_PLAYBACK_PAUSED_GENERATI_7F91F9D4,
                "WAV playback paused generation=%u frame=%llu resources=released",
                (unsigned)generation,
                (unsigned long long)(data_offset / sizeof(int16_t)));

            suspended = true;
            break;
        }

        /* Normal EOF. */
        break;
    }

    const esp_err_t terminal_cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (terminal_cleanup_result != ESP_OK))
    {
        result = terminal_cleanup_result;
        cancelled = false;
    }
    else if ((result != ESP_OK) && (terminal_cleanup_result != ESP_OK))
    {
        APP_LOGE(
            TAG, WAV_CLEANUP_ALSO_FAILED_S_1E2F345D,
            "WAV cleanup also failed: %s",
            esp_err_to_name(terminal_cleanup_result));
    }

    audio_manager_diagnostics_t diagnostics_after = {0};
    audio_manager_snapshot_diagnostics(&diagnostics_after);

    APP_LOGI(
        TAG, WAV_DIAG_RESULT_S_CANCELLED_832A9A3F,
        "WAV_DIAG result=%s cancelled=%s expected_bytes=%u duration=%ums fixed_gain_q16=%u output_peak=%u read_bytes=%llu streamed_bytes=%llu raw_reads=%u raw_read_fail=%u max_raw_read_us=%u prefetch_block=%u prefetch_fills=%u prefetch_fill_fail=%u max_prefetch_fill_us=%u sd_resume_offset=%llu sd_resume_attempt=%u sd_resume_ok=%u sd_resume_wait=%ums initial_wait=%ums boundary_wait=%ums prefetch_starve=%u reader_hwm=%u elapsed=%ums tx_requested=%llu tx_written=%llu tx_q_ovf=%u tx_timeout=%u tx_partial=%u max_tx_us=%u",
        esp_err_to_name(result),
        cancelled ? "yes" : "no",
        (unsigned)expected_data_bytes,
        (unsigned)expected_duration_ms,
        (unsigned)metrics.wav.fixed_scale_gain_q16,
        (unsigned)metrics.wav.output_peak_pcm16,
        (unsigned long long)metrics.wav.data_bytes_read,
        (unsigned long long)metrics.wav.data_bytes_streamed,
        (unsigned)metrics.wav.read_count,
        (unsigned)metrics.wav.read_failure_count,
        (unsigned)metrics.wav.max_wav_read_duration_us,
        (unsigned)metrics.wav.prefetch_block_bytes,
        (unsigned)metrics.wav.prefetch_blocks_filled,
        (unsigned)metrics.wav.prefetch_fill_failure_count,
        (unsigned)metrics.wav.max_prefetch_fill_duration_us,
        (unsigned long long)metrics.wav.prefetch_recovery_data_offset,
        (unsigned)metrics.wav.prefetch_recovery_attempt_count,
        (unsigned)metrics.wav.prefetch_recovery_success_count,
        (unsigned)metrics.wav.prefetch_recovery_wait_ms,
        (unsigned)metrics.wav.initial_prefetch_wait_ms,
        (unsigned)metrics.wav.prefetch_wait_ms,
        (unsigned)metrics.wav.prefetch_starvation_count,
        (unsigned)metrics.wav.prefetch_task_stack_high_water,
        (unsigned)metrics.wav.playback_elapsed_ms,
        (unsigned long long)(diagnostics_after.tx_bytes_requested -
                             diagnostics_before.tx_bytes_requested),
        (unsigned long long)(diagnostics_after.tx_bytes_written -
                             diagnostics_before.tx_bytes_written),
        (unsigned)(diagnostics_after.tx_queue_overflow_count -
                   diagnostics_before.tx_queue_overflow_count),
        (unsigned)(diagnostics_after.tx_timeout_count -
                   diagnostics_before.tx_timeout_count),
        (unsigned)(diagnostics_after.tx_partial_write_count -
                   diagnostics_before.tx_partial_write_count),
        (unsigned)diagnostics_after.max_tx_write_duration_us);

    if (suspended && (result == ESP_OK))
    {
        audio_manager_suspend_operation();
        audio_manager_notify_status_changed();
        if (audio_manager_consume_resume_requested())
        {
            const esp_err_t resume_result =
                audio_manager_queue_retained_playback(0U, false);
            if (resume_result != ESP_OK)
            {
                APP_LOGW(TAG, RAPID_RESUME_QUEUE_FAILED_6954B731,
                         "WAV rapid resume queue failed: %s",
                         esp_err_to_name(resume_result));
            }
        }
        return;
    }

    audio_manager_complete_playback_control(result);
    audio_manager_clear_resume_context();

    bool status_updated = false;
    if (audio_manager_take_status_mutex("storing WAV playback result"))
    {
        s_runtime.status.last_error = result;
        if (result != ESP_OK)
        {
            ++s_runtime.status.wav_playback_failed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_ERROR;
        }
        else if (cancelled)
        {
            ++s_runtime.status.wav_playback_cancelled;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }
        else
        {
            ++s_runtime.status.wav_playback_completed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }

        audio_manager_refresh_diagnostics_locked();
        status_updated = true;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    if (!status_updated)
    {
        /* Best-effort recovery for a failed terminal-status store. */
        audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    }

    audio_manager_finish_wav_operation();

    if (status_updated)
    {
        audio_manager_notify_status_changed();
    }

    if (result != ESP_OK)
    {
        APP_LOGE(TAG, WAV_PLAYBACK_FAILED_S_C73F9DC7, "WAV playback failed: %s", esp_err_to_name(result));
        if (status_updated)
        {
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        }
    }
    else if (cancelled)
    {
        APP_LOGI(TAG, WAV_PLAYBACK_CANCELLED_A0B2FA79, "WAV playback cancelled");
    }
    else
    {
        APP_LOGI(TAG, WAV_PLAYBACK_COMPLETED_0F7BEC92, "WAV playback completed");
    }
}

static void audio_manager_handle_pcm_stream_command(uint32_t generation)
{
    APP_LOGI(TAG, PCM16_STREAM_PLAYBACK_GENERA_F537C991,
             "========== PCM16 STREAM PLAYBACK generation=%u ==========",
             (unsigned)generation);

    if (audio_manager_take_status_mutex("starting PCM stream playback"))
    {
        ++s_runtime.status.pcm_stream_playback_started;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    bool cancelled = audio_manager_cancel_is_requested();
    esp_err_t result = ESP_OK;
    audio_manager_pcm_stream_status_t before = {0};
    if (!cancelled)
    {
        result = audio_manager_pcm_stream_get_status(generation, &before);
        if ((result == ESP_OK) && (before.aborted || !before.active))
        {
            cancelled = true;
        }
    }

    if ((result == ESP_OK) && !cancelled)
    {
        result = audio_manager_select_pcm_stream_playback_source(generation);
    }
    if ((result == ESP_OK) && !cancelled)
    {
        audio_cycle_metrics_t metrics = {0};
        result = playback_once(
            &s_runtime.playback_source,
            &metrics,
            &cancelled);
    }

    const esp_err_t cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (cleanup_result != ESP_OK))
    {
        result = cleanup_result;
        cancelled = false;
    }
    else if ((result != ESP_OK) && (cleanup_result != ESP_OK))
    {
        APP_LOGE(TAG, PCM_STREAM_CLEANUP_ALSO_FAIL_EEBB1848,
                 "PCM stream cleanup also failed: %s",
                 esp_err_to_name(cleanup_result));
    }

    audio_manager_pcm_stream_status_t terminal = {0};
    const esp_err_t terminal_status_result = audio_manager_pcm_stream_get_status(
        generation,
        &terminal);
    audio_manager_pcm_stream_close_from_owner(generation);

    APP_LOGI(TAG, PCM_STREAM_DIAG_GENERATION_U_7D243BC2,
             "PCM_STREAM_DIAG generation=%u result=%s cancelled=%s accepted=%llu played=%llu queued=%u high_water=%u full=%u starvation=%u",
             (unsigned)generation,
             esp_err_to_name(result),
             cancelled ? "yes" : "no",
             (unsigned long long)terminal.accepted_samples,
             (unsigned long long)terminal.played_samples,
             (unsigned)terminal.queued_samples,
             (unsigned)terminal.high_water_samples,
             (unsigned)terminal.full_count,
             (unsigned)terminal.starvation_count);
    if (terminal_status_result != ESP_OK)
    {
        APP_LOGW(TAG, PCM_STREAM_TERMINAL_DIAGNOST_9E20B2A2,
                 "PCM stream terminal diagnostics unavailable generation=%u error=%s",
                 (unsigned)generation,
                  esp_err_to_name(terminal_status_result));
    }

    audio_manager_complete_playback_control(result);
    /* A Xiaozhi PCM stream may temporarily use TX while a local source is
     * suspended for PTT. Restore that copied local PAUSED identity after the
     * non-seekable stream terminates; no FILE/I2S resource is retained. */
    audio_manager_restore_retained_playback_status();

    bool status_updated = false;
    if (audio_manager_take_status_mutex("storing PCM stream result"))
    {
        s_runtime.status.last_error = result;
        if (result != ESP_OK)
        {
            ++s_runtime.status.pcm_stream_playback_failed;
            /* A failed/non-seekable voice stream has already released I2S.
             * If it temporarily displaced a retained local source, keep the
             * manager operation slot recoverable so the voice-turn policy can
             * attempt its single generation-guarded resume. The voice failure
             * remains visible in last_error/counters. */
            s_runtime.status.state = s_runtime.playback_resume.valid
                ? AUDIO_MANAGER_STATE_IDLE
                : AUDIO_MANAGER_STATE_ERROR;
        }
        else if (cancelled)
        {
            ++s_runtime.status.pcm_stream_playback_cancelled;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }
        else
        {
            ++s_runtime.status.pcm_stream_playback_completed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }
        audio_manager_refresh_diagnostics_locked();
        status_updated = true;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    audio_manager_finish_operation();

    if (status_updated)
    {
        audio_manager_notify_status_changed();
    }
    else
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    }

    if (result != ESP_OK)
    {
        APP_LOGE(TAG, PCM_STREAM_PLAYBACK_FAILED_G_CA4E5C14,
                 "PCM stream playback failed generation=%u: %s",
                 (unsigned)generation,
                 esp_err_to_name(result));
        if (status_updated)
        {
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        }
    }
    else if (cancelled)
    {
        APP_LOGI(TAG, PCM_STREAM_PLAYBACK_CANCELLE_54B13A30,
                 "PCM stream playback cancelled generation=%u",
                 (unsigned)generation);
    }
    else
    {
        APP_LOGI(TAG, PCM_STREAM_PLAYBACK_COMPLETE_AF4DB41E,
                 "PCM stream playback completed generation=%u",
                 (unsigned)generation);
    }
}

/* Static Functions: Manager Task / Regression ----------------------------- */
static void log_cycle_diagnostics(
    uint32_t cycle,
    const audio_manager_diagnostics_t *before)
{
    if (before == NULL)
    {
        return;
    }

    audio_manager_diagnostics_t after = {0};
    audio_manager_snapshot_diagnostics(&after);

    const uint32_t rx_overflow_delta =
        after.rx_overflow_count - before->rx_overflow_count;
    const uint32_t rx_timeout_delta =
        after.rx_timeout_count - before->rx_timeout_count;
    const uint32_t tx_overflow_delta =
        after.tx_queue_overflow_count - before->tx_queue_overflow_count;
    const uint32_t tx_timeout_delta =
        after.tx_timeout_count - before->tx_timeout_count;
    const uint32_t tx_partial_delta =
        after.tx_partial_write_count - before->tx_partial_write_count;
    const uint64_t rx_bytes_delta =
        after.rx_bytes_read - before->rx_bytes_read;
    const uint64_t tx_bytes_delta =
        after.tx_bytes_written - before->tx_bytes_written;

    APP_LOGI(
        TAG, CYCLE_DIAG_U_RX_BYTES_19E56C82,
        "CYCLE_DIAG #%u rx_bytes=%llu tx_bytes=%llu rx_ovf=%u rx_timeout=%u tx_q_ovf=%u tx_timeout=%u tx_partial=%u max_rx_us=%u max_tx_us=%u stack_hwm=%u_bytes",
        (unsigned)cycle,
        (unsigned long long)rx_bytes_delta,
        (unsigned long long)tx_bytes_delta,
        (unsigned)rx_overflow_delta,
        (unsigned)rx_timeout_delta,
        (unsigned)tx_overflow_delta,
        (unsigned)tx_timeout_delta,
        (unsigned)tx_partial_delta,
        (unsigned)after.max_rx_read_duration_us,
        (unsigned)after.max_tx_write_duration_us,
        (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void audio_manager_run_stability_iteration(void)
{
    uint32_t cycle = 0U;

    if (audio_manager_take_status_mutex("starting cycle"))
    {
        cycle = ++s_runtime.status.cycles_started;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    audio_manager_diagnostics_t diagnostics_before = {0};
    audio_manager_snapshot_diagnostics(&diagnostics_before);

    APP_LOGI(
        TAG, AUDIO_MANAGER_CYCLE_U_36080E93,
        "========== AUDIO MANAGER CYCLE #%u ==========",
        (unsigned)cycle);

    audio_cycle_metrics_t metrics;
    const esp_err_t result = run_cycle(&metrics);

    if (audio_manager_take_status_mutex("storing cycle diagnostics"))
    {
        s_runtime.status.last_samples_recorded = metrics.samples_recorded;
        s_runtime.status.last_error = result;
        audio_manager_refresh_diagnostics_locked();

        if (result == ESP_OK)
        {
            ++s_runtime.status.cycles_completed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_IDLE;
        }
        else
        {
            ++s_runtime.status.cycles_failed;
            s_runtime.status.state = AUDIO_MANAGER_STATE_ERROR;
        }

        xSemaphoreGive(s_runtime.status_mutex);
    }

    /* Publish result/counters even when state remains IDLE after success. */
    audio_manager_notify_status_changed();

    if (result == ESP_OK)
    {
        APP_LOGI(
            TAG, CYCLE_U_PASS_SAMPLES_U_7A4FF6AA,
            "CYCLE #%u PASS samples=%u",
            (unsigned)cycle,
            (unsigned)metrics.samples_recorded);
    }
    else
    {
        APP_LOGE(
            TAG, CYCLE_U_FAIL_S_B3098400,
            "CYCLE #%u FAIL: %s",
            (unsigned)cycle,
            esp_err_to_name(result));

        /* Keep soak useful after transient faults: cleanup, report, retry. */
        const esp_err_t cleanup_result = force_cycle_cleanup();
        if (cleanup_result != ESP_OK)
        {
            APP_LOGE(
                TAG, CYCLE_CLEANUP_FAILED_S_0A397BA9,
                "Cycle cleanup failed: %s",
                esp_err_to_name(cleanup_result));
        }
    }

    log_cycle_diagnostics(cycle, &diagnostics_before);
    log_heap_state("cycle_end");

    if (result != ESP_OK)
    {
        /* Preserve last_error/counters but recover lifecycle for next cycle. */
        audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    }
}

static void audio_manager_task(void *argument)
{
    (void)argument;

    const bool stability_mode = audio_manager_stability_mode_enabled();
    const bool mixed_stress_mode = audio_manager_mixed_stress_mode_enabled();
    APP_LOGI(
        TAG, AUDIO_MANAGER_TASK_STARTED_M_64BC47F1,
        "Audio manager task started: mode=%s priority=%u volume=%u/100",
        stability_mode
            ? (mixed_stress_mode ? "golden_wav_stress" : "golden_stability")
            : "production_idle",
        (unsigned)AUDIO_MANAGER_TASK_PRIORITY,
        (unsigned)s_runtime.config.playback_volume_percent);

    audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    xEventGroupSetBits(
        s_runtime.lifecycle_events,
        AUDIO_MANAGER_TASK_READY_BIT);

    bool task_done = false;
    while (!task_done)
    {
        if (stability_mode)
        {
            if (audio_manager_shutdown_is_requested())
            {
                break;
            }

            if (mixed_stress_mode)
            {
                /*
                 * Drain a command accepted at the preceding inter-cycle
                 * window before reserving I2S for the next golden cycle.
                 */
                audio_manager_command_t command = {0};
                if (xQueueReceive(
                        s_runtime.command_queue,
                        &command,
                        0U) == pdTRUE)
                {
                    if (command.kind == AUDIO_MANAGER_COMMAND_PLAY_WAV)
                    {
                        audio_manager_handle_wav_command(
                            command.wav_path,
                            command.retained_resume,
                            command.restart_from_beginning);
                    }
                    else if (command.kind == AUDIO_MANAGER_COMMAND_SHUTDOWN)
                    {
                        task_done = true;
                    }
                    else
                    {
                        APP_LOGE(
                            TAG, UNKNOWN_AUDIO_COMMAND_D_4B91134B,
                            "Unknown audio command: %d",
                            (int)command.kind);
                    }
                    continue;
                }

                if (!audio_manager_try_begin_stability_operation())
                {
                    /* A producer won the slot immediately after the poll. */
                    vTaskDelay(1U);
                    continue;
                }
            }

            audio_manager_run_stability_iteration();

            if (mixed_stress_mode)
            {
                /* Only the completed cycle releases the command/I2S slot. */
                audio_manager_finish_operation();
            }

            audio_manager_command_t command = {0};
            if (xQueueReceive(
                    s_runtime.command_queue,
                    &command,
                    pdMS_TO_TICKS(AUDIO_MANAGER_INTER_CYCLE_DELAY_MS)) ==
                    pdTRUE)
            {
                if (mixed_stress_mode &&
                    (command.kind == AUDIO_MANAGER_COMMAND_PLAY_WAV))
                {
                    audio_manager_handle_wav_command(
                        command.wav_path,
                        command.retained_resume,
                        command.restart_from_beginning);
                }
                else if (command.kind == AUDIO_MANAGER_COMMAND_SHUTDOWN)
                {
                    task_done = true;
                }
                else
                {
                    APP_LOGE(
                        TAG, UNEXPECTED_AUDIO_COMMAND_IN_89611EFE,
                        "Unexpected audio command in golden mode: %d",
                        (int)command.kind);
                }
            }
            continue;
        }

        audio_manager_command_t command = {0};
        if (xQueueReceive(
                s_runtime.command_queue,
                &command,
                pdMS_TO_TICKS(AUDIO_MANAGER_COMMAND_POLL_MS)) == pdTRUE)
        {
            switch (command.kind)
            {
                case AUDIO_MANAGER_COMMAND_RECORD_FIXED:
                    audio_manager_handle_record_command(false);
                    break;

                case AUDIO_MANAGER_COMMAND_RECORD_MANUAL:
                    audio_manager_handle_record_command(true);
                    break;

                case AUDIO_MANAGER_COMMAND_PLAY_RECORDED:
                    audio_manager_handle_recorded_playback_command(
                        command.retained_resume,
                        command.restart_from_beginning);
                    break;

                case AUDIO_MANAGER_COMMAND_PLAY_WAV:
                    audio_manager_handle_wav_command(
                        command.wav_path,
                        command.retained_resume,
                        command.restart_from_beginning);
                    break;

                case AUDIO_MANAGER_COMMAND_PLAY_PCM16_STREAM:
                    audio_manager_handle_pcm_stream_command(
                        command.stream_generation);
                    break;

                case AUDIO_MANAGER_COMMAND_SHUTDOWN:
                    task_done = true;
                    break;

                default:
                    APP_LOGE(
                        TAG, UNKNOWN_AUDIO_COMMAND_D_DC41E118,
                        "Unknown audio command: %d",
                        (int)command.kind);
                    break;
            }
        }
        else if (audio_manager_shutdown_is_requested())
        {
            /* Shutdown flag is the fallback when its queue send was full. */
            task_done = true;
        }
    }

    const esp_err_t cleanup_result = force_cycle_cleanup();
    /* A paused local source owns no live resources, but its copied resume
     * identity must not survive a manager stop/start lifecycle. */
    audio_manager_clear_resume_context();
    esp_err_t task_result = cleanup_result;
    bool final_status_updated = false;

    if (audio_manager_take_status_mutex("storing task shutdown result"))
    {
        if (cleanup_result != ESP_OK)
        {
            s_runtime.status.last_error = cleanup_result;
        }
        s_runtime.status.state = AUDIO_MANAGER_STATE_INITIALIZED;
        audio_manager_refresh_diagnostics_locked();
        s_runtime.task_exit_result = task_result;
        final_status_updated = true;
        xSemaphoreGive(s_runtime.status_mutex);
    }
    else
    {
        if (task_result == ESP_OK)
        {
            task_result = ESP_ERR_TIMEOUT;
        }
        s_runtime.task_exit_result = task_result;
    }

    if (final_status_updated)
    {
        audio_manager_notify_status_changed();
    }

    if (audio_manager_take_status_mutex("finishing task shutdown"))
    {
        s_runtime.task_handle = NULL;
        audio_manager_reset_control();
        xEventGroupSetBits(
            s_runtime.lifecycle_events,
            AUDIO_MANAGER_TASK_STOPPED_BIT);
        xSemaphoreGive(s_runtime.status_mutex);
    }
    else
    {
        if (task_result == ESP_OK)
        {
            task_result = ESP_ERR_TIMEOUT;
            s_runtime.task_exit_result = task_result;
        }
        s_runtime.task_handle = NULL;
        audio_manager_reset_control();
        xEventGroupSetBits(
            s_runtime.lifecycle_events,
            AUDIO_MANAGER_TASK_STOPPED_BIT);
    }

    APP_LOGI(
        TAG, AUDIO_MANAGER_TASK_STOPPED_S_5859C682,
        "Audio manager task stopped: %s",
        esp_err_to_name(task_result));
    vTaskDelete(NULL);
}

/* Public API --------------------------------------------------------------- */
audio_manager_config_t audio_manager_default_config(void)
{
    return (audio_manager_config_t) {
        .record_duration_seconds = AUDIO_MANAGER_DEFAULT_RECORD_SECONDS,
        .playback_volume_percent = AUDIO_MANAGER_DEFAULT_VOLUME_PERCENT,
    };
}

esp_err_t audio_manager_init(const audio_manager_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if ((config->record_duration_seconds == 0U) ||
        (config->playback_volume_percent > AUDIO_DSP_VOLUME_PERCENT_MAX))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t fixed_record_sample_count =
        (size_t)AUDIO_MANAGER_SAMPLE_RATE_HZ *
        (size_t)config->record_duration_seconds;
    const size_t manual_record_sample_limit =
        (size_t)AUDIO_MANAGER_SAMPLE_RATE_HZ *
        (size_t)AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS;

    if (((fixed_record_sample_count / AUDIO_MANAGER_SAMPLE_RATE_HZ) !=
         config->record_duration_seconds) ||
        ((manual_record_sample_limit / AUDIO_MANAGER_SAMPLE_RATE_HZ) !=
         AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t sample_capacity =
        (fixed_record_sample_count > manual_record_sample_limit)
            ? fixed_record_sample_count
            : manual_record_sample_limit;

    if (sample_capacity > (SIZE_MAX / sizeof(int32_t)))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.config = *config;
    s_runtime.sample_capacity = sample_capacity;
    s_runtime.fixed_record_sample_count = fixed_record_sample_count;
    s_runtime.manual_record_sample_limit = manual_record_sample_limit;
    s_runtime.recording_bytes = sample_capacity * sizeof(int32_t);

    s_runtime.status_mutex = xSemaphoreCreateMutex();
    if (s_runtime.status_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_runtime.pcm_stream_mutex = xSemaphoreCreateMutex();
    if (s_runtime.pcm_stream_mutex == NULL)
    {
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_runtime.command_queue = xQueueCreate(
        AUDIO_MANAGER_COMMAND_QUEUE_LENGTH,
        sizeof(audio_manager_command_t));
    if (s_runtime.command_queue == NULL)
    {
        vSemaphoreDelete(s_runtime.pcm_stream_mutex);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.pcm_stream_mutex = NULL;
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_runtime.lifecycle_events = xEventGroupCreate();
    if (s_runtime.lifecycle_events == NULL)
    {
        vQueueDelete(s_runtime.command_queue);
        vSemaphoreDelete(s_runtime.pcm_stream_mutex);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.command_queue = NULL;
        s_runtime.pcm_stream_mutex = NULL;
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = hold_amplifier_data_low();
    if (result != ESP_OK)
    {
        vEventGroupDelete(s_runtime.lifecycle_events);
        vQueueDelete(s_runtime.command_queue);
        vSemaphoreDelete(s_runtime.pcm_stream_mutex);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.lifecycle_events = NULL;
        s_runtime.command_queue = NULL;
        s_runtime.pcm_stream_mutex = NULL;
        s_runtime.status_mutex = NULL;
        return result;
    }

    log_heap_state("before_audio_alloc");

    s_runtime.recording_pcm24 = (int32_t *)heap_caps_malloc(
        s_runtime.recording_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_runtime.dsp_workspace = (audio_dsp_workspace_t *)heap_caps_malloc(
        sizeof(audio_dsp_workspace_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_runtime.pcm_stream_storage = (int16_t *)heap_caps_malloc(
        AUDIO_MANAGER_PCM_STREAM_RING_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if ((s_runtime.recording_pcm24 == NULL) ||
        (s_runtime.dsp_workspace == NULL) ||
        (s_runtime.pcm_stream_storage == NULL) ||
        !esp_ptr_external_ram(s_runtime.recording_pcm24) ||
        !esp_ptr_external_ram(s_runtime.dsp_workspace) ||
        !esp_ptr_external_ram(s_runtime.pcm_stream_storage))
    {
        heap_caps_free(s_runtime.recording_pcm24);
        heap_caps_free(s_runtime.dsp_workspace);
        heap_caps_free(s_runtime.pcm_stream_storage);
        s_runtime.recording_pcm24 = NULL;
        s_runtime.dsp_workspace = NULL;
        s_runtime.pcm_stream_storage = NULL;
        vEventGroupDelete(s_runtime.lifecycle_events);
        vQueueDelete(s_runtime.command_queue);
        vSemaphoreDelete(s_runtime.pcm_stream_mutex);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.lifecycle_events = NULL;
        s_runtime.command_queue = NULL;
        s_runtime.pcm_stream_mutex = NULL;
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(s_runtime.recording_pcm24, 0, s_runtime.recording_bytes);
    audio_dsp_workspace_init(s_runtime.dsp_workspace);
    result = audio_manager_pcm_stream_core_init(
        &s_runtime.pcm_stream,
        s_runtime.pcm_stream_storage,
        AUDIO_MANAGER_PCM_STREAM_RING_SAMPLES);
    if (result != ESP_OK)
    {
        heap_caps_free(s_runtime.recording_pcm24);
        heap_caps_free(s_runtime.dsp_workspace);
        heap_caps_free(s_runtime.pcm_stream_storage);
        s_runtime.recording_pcm24 = NULL;
        s_runtime.dsp_workspace = NULL;
        s_runtime.pcm_stream_storage = NULL;
        vEventGroupDelete(s_runtime.lifecycle_events);
        vQueueDelete(s_runtime.command_queue);
        vSemaphoreDelete(s_runtime.pcm_stream_mutex);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.lifecycle_events = NULL;
        s_runtime.command_queue = NULL;
        s_runtime.pcm_stream_mutex = NULL;
        s_runtime.status_mutex = NULL;
        return result;
    }

    audio_manager_reset_diagnostics();
    audio_manager_reset_control();

    s_runtime.initialized = true;
    s_runtime.task_exit_result = ESP_OK;
    s_runtime.recorded_audio_valid = false;
    s_runtime.recorded_sample_count = 0U;
    s_runtime.status = (audio_manager_status_t) {
        .state = AUDIO_MANAGER_STATE_INITIALIZED,
        .recorded_audio_available = false,
        .last_error = ESP_OK,
    };

    APP_LOGI(TAG, MESSAGE_69F65059, "================================================");
    APP_LOGI(TAG, AUDIO_MANAGER_INITIALIZED_7B71541D, "Audio manager initialized");
    APP_LOGI(
        TAG, PINS_BCLK_GPIO_D_WS_33446E3D,
        "Pins BCLK=GPIO%d WS=GPIO%d MIC_DIN=GPIO%d SPK_DOUT=GPIO%d",
        AUDIO_GPIO_BCLK,
        AUDIO_GPIO_WS,
        AUDIO_GPIO_MIC_DIN,
        AUDIO_GPIO_SPK_DOUT);
    APP_LOGI(
        TAG, CONFIG_SAMPLE_RATE_U_FIXED_3E4198E4,
        "Config sample_rate=%u fixed_record=%us manual_max=%us capacity_samples=%u PCM24_PSRAM=%uB volume=%u/100 DMA=%ux%u",
        (unsigned)AUDIO_MANAGER_SAMPLE_RATE_HZ,
        (unsigned)config->record_duration_seconds,
        (unsigned)AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS,
        (unsigned)s_runtime.sample_capacity,
        (unsigned)s_runtime.recording_bytes,
        (unsigned)config->playback_volume_percent,
        (unsigned)AUDIO_MANAGER_DMA_DESC_NUM,
        (unsigned)AUDIO_MANAGER_FRAMES_PER_BLOCK);
    APP_LOGI(
        TAG, WAV_PREFETCH_US_SLOT_UB_DC966F5C,
        "WAV prefetch=%us slot=%uB slots=%u PSRAM_total=%uB reader_priority=%u",
        (unsigned)CONFIG_AUDIO_MANAGER_WAV_PREFETCH_SECONDS,
        (unsigned)AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES,
        (unsigned)AUDIO_WAV_PREFETCH_SLOT_COUNT,
        (unsigned)(AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES *
                   AUDIO_WAV_PREFETCH_SLOT_COUNT),
        (unsigned)AUDIO_MANAGER_WAV_PREFETCH_READER_PRIORITY);
    APP_LOGI(
        TAG, PCM_STREAM_RING_U_SAMPLES_64B75EE7,
        "PCM stream ring=%u samples=%uB prefill=%u samples=%ums",
        (unsigned)AUDIO_MANAGER_PCM_STREAM_RING_SAMPLES,
        (unsigned)(AUDIO_MANAGER_PCM_STREAM_RING_SAMPLES * sizeof(int16_t)),
        (unsigned)AUDIO_MANAGER_PCM_STREAM_PREFILL_SAMPLES,
        (unsigned)((AUDIO_MANAGER_PCM_STREAM_PREFILL_SAMPLES * 1000U) /
                   AUDIO_MANAGER_SAMPLE_RATE_HZ));
    APP_LOGI(
        TAG, DSP_HPF80X2_LPF6KX2_ADAPTIVE_E25F8B02,
        "DSP HPF80x2 + LPF6kx2 + adaptive NS + 16x speaker conditioning + limiter");
    APP_LOGI(TAG, MESSAGE_69F65059, "================================================");

    log_heap_state("after_audio_alloc");
    return ESP_OK;
}

esp_err_t audio_manager_register_status_callback(
    audio_manager_status_callback_t callback,
    void *user_context)
{
    if (!s_runtime.initialized || (s_runtime.status_mutex == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_runtime.status_mutex,
            pdMS_TO_TICKS(AUDIO_MANAGER_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_runtime.status_callback = callback;
    s_runtime.status_callback_context = user_context;

    audio_manager_refresh_diagnostics_locked();
    audio_manager_status_t status_snapshot = s_runtime.status;
    xSemaphoreGive(s_runtime.status_mutex);

    /* Seed GUI/adapters immediately with the current lifecycle state. */
    if (callback != NULL)
    {
        callback(&status_snapshot, user_context);
    }

    return ESP_OK;
}

esp_err_t audio_manager_start(void)
{
    if (!s_runtime.initialized ||
        (s_runtime.status_mutex == NULL) ||
        (s_runtime.pcm_stream_mutex == NULL) ||
        (s_runtime.command_queue == NULL) ||
        (s_runtime.lifecycle_events == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_status_mutex("starting manager task"))
    {
        return ESP_ERR_TIMEOUT;
    }

    bool already_running;
    portENTER_CRITICAL(&s_control_lock);
    already_running = s_control.task_running;
    if (!already_running && (s_runtime.task_handle == NULL))
    {
        s_control.task_running = true;
        s_control.shutdown_requested = false;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
        s_control.operation = AUDIO_MANAGER_OPERATION_NONE;
    }
    portEXIT_CRITICAL(&s_control_lock);

    if (already_running || (s_runtime.task_handle != NULL))
    {
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    (void)xQueueReset(s_runtime.command_queue);
    (void)xEventGroupClearBits(
        s_runtime.lifecycle_events,
        AUDIO_MANAGER_TASK_READY_BIT | AUDIO_MANAGER_TASK_STOPPED_BIT);
    s_runtime.task_exit_result = ESP_OK;

    const BaseType_t result = xTaskCreate(
        audio_manager_task,
        AUDIO_MANAGER_TASK_NAME,
        AUDIO_MANAGER_TASK_STACK_SIZE,
        NULL,
        AUDIO_MANAGER_TASK_PRIORITY,
        &s_runtime.task_handle);

    if (result != pdPASS)
    {
        s_runtime.task_handle = NULL;
        audio_manager_reset_control();
        s_runtime.status.state = AUDIO_MANAGER_STATE_INITIALIZED;
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_runtime.status_mutex);

    const EventBits_t ready_bits = xEventGroupWaitBits(
        s_runtime.lifecycle_events,
        AUDIO_MANAGER_TASK_READY_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(AUDIO_MANAGER_TASK_START_TIMEOUT_MS));
    if ((ready_bits & AUDIO_MANAGER_TASK_READY_BIT) == 0U)
    {
        APP_LOGE(TAG, MANAGER_TASK_DID_NOT_REACH_1502025B, "Manager task did not reach IDLE before timeout");
        (void)audio_manager_stop();
        return ESP_ERR_TIMEOUT;
    }

    APP_LOGI(TAG, STARTED_AND_READY_FOR_COMMAN_D6A037B6, "Started and ready for commands");

    return ESP_OK;
}

esp_err_t audio_manager_record(void)
{
    return audio_manager_queue_simple_operation(
        AUDIO_MANAGER_COMMAND_RECORD_FIXED,
        AUDIO_MANAGER_OPERATION_RECORD_FIXED,
        false,
        "queueing fixed recording");
}

esp_err_t audio_manager_start_recording(void)
{
    return audio_manager_queue_simple_operation(
        AUDIO_MANAGER_COMMAND_RECORD_MANUAL,
        AUDIO_MANAGER_OPERATION_RECORD_MANUAL,
        false,
        "queueing manual recording");
}

esp_err_t audio_manager_stop_recording(void)
{
    if (!s_runtime.initialized || (s_runtime.status_mutex == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_status_mutex("stopping manual recording"))
    {
        return ESP_ERR_TIMEOUT;
    }

    bool requested = false;
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.task_running &&
        !s_control.shutdown_requested &&
        (s_control.operation == AUDIO_MANAGER_OPERATION_RECORD_MANUAL))
    {
        s_control.record_stop_requested = true;
        requested = true;
    }
    portEXIT_CRITICAL(&s_control_lock);

    xSemaphoreGive(s_runtime.status_mutex);
    return requested ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t audio_manager_play_recorded(void)
{
    return audio_manager_queue_simple_operation(
        AUDIO_MANAGER_COMMAND_PLAY_RECORDED,
        AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK,
        true,
        "queueing recorded playback");
}

esp_err_t audio_manager_play_wav(const char *path)
{
    if (path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t path_length = strnlen(
        path,
        AUDIO_MANAGER_WAV_PATH_MAX_BYTES);
    if ((path_length == 0U) ||
        (path_length >= AUDIO_MANAGER_WAV_PATH_MAX_BYTES) ||
        !audio_wav_path_is_valid(path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_runtime.initialized ||
        (s_runtime.status_mutex == NULL) ||
        (s_runtime.command_queue == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_manager_command_t command = {
        .kind = AUDIO_MANAGER_COMMAND_PLAY_WAV,
    };
    memcpy(command.wav_path, path, path_length + 1U);

    if (!audio_manager_take_status_mutex("queueing WAV playback"))
    {
        return ESP_ERR_TIMEOUT;
    }

    bool accepted = false;
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.task_running &&
        !s_control.shutdown_requested &&
        (s_control.operation == AUDIO_MANAGER_OPERATION_NONE) &&
        (s_runtime.status.state == AUDIO_MANAGER_STATE_IDLE) &&
        (s_control.playback_status.state ==
         AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE))
    {
        s_control.operation = AUDIO_MANAGER_OPERATION_WAV;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
        audio_manager_begin_playback_control_locked(
            AUDIO_MANAGER_PLAYBACK_SOURCE_WAV,
            true,
            0U);
        accepted = true;
    }
    portEXIT_CRITICAL(&s_control_lock);

    if (!accepted)
    {
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_runtime.command_queue, &command, 0U) != pdTRUE)
    {
        audio_manager_abort_playback_control_begin();
        audio_manager_finish_operation();
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_runtime.status_mutex);
    return ESP_OK;
}

static esp_err_t audio_manager_queue_retained_playback(
    uint32_t expected_generation,
    bool restart_from_beginning)
{
    if (!s_runtime.initialized || (s_runtime.status_mutex == NULL) ||
        (s_runtime.command_queue == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_manager_take_status_mutex("queueing retained playback"))
    {
        return ESP_ERR_TIMEOUT;
    }

    audio_manager_command_t command = {0};
    audio_manager_playback_status_t previous = {0};
    audio_manager_operation_t operation = AUDIO_MANAGER_OPERATION_NONE;
    bool accepted = false;

    portENTER_CRITICAL(&s_control_lock);
    previous = s_control.playback_status;
    if (s_control.task_running && !s_control.shutdown_requested &&
        (s_control.operation == AUDIO_MANAGER_OPERATION_NONE) &&
        (s_runtime.status.state == AUDIO_MANAGER_STATE_IDLE) &&
        s_runtime.playback_resume.valid &&
        (s_control.playback_status.state ==
         AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED) &&
        ((expected_generation == 0U) ||
         (expected_generation == s_runtime.playback_resume.generation)) &&
        (s_control.playback_status.generation ==
         s_runtime.playback_resume.generation))
    {
        audio_manager_playback_control_state_t next =
            s_control.playback_status.state;
        const audio_manager_playback_control_event_t event =
            restart_from_beginning
                ? AUDIO_MANAGER_PLAYBACK_EVENT_RESTART_REQUESTED
                : AUDIO_MANAGER_PLAYBACK_EVENT_RESUME_REQUESTED;
        if (audio_manager_playback_control_transition(
                s_control.playback_status.state, event, &next) == ESP_OK)
        {
            operation =
                (s_runtime.playback_resume.kind ==
                 AUDIO_PLAYBACK_SOURCE_WAV_PCM16)
                    ? AUDIO_MANAGER_OPERATION_WAV
                    : AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK;
            s_control.operation = operation;
            s_control.cancel_requested = false;
            s_control.pause_requested = false;
            s_control.resume_requested = false;
            s_control.restart_requested = false;
            s_control.playback_status.state = next;
            s_control.playback_status.pause_reason =
                AUDIO_MANAGER_PLAYBACK_PAUSE_NONE;
            s_control.playback_status.last_action = restart_from_beginning
                ? AUDIO_MANAGER_PLAYBACK_ACTION_RESTART
                : AUDIO_MANAGER_PLAYBACK_ACTION_RESUME;
            s_control.playback_status.last_control_result = ESP_OK;
            if (restart_from_beginning)
            {
                s_control.playback_status.position_frames = 0U;
            }
            accepted = true;
        }
    }
    portEXIT_CRITICAL(&s_control_lock);

    if (!accepted)
    {
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    command.kind = (operation == AUDIO_MANAGER_OPERATION_WAV)
        ? AUDIO_MANAGER_COMMAND_PLAY_WAV
        : AUDIO_MANAGER_COMMAND_PLAY_RECORDED;
    command.retained_resume = true;
    command.restart_from_beginning = restart_from_beginning;
    if (operation == AUDIO_MANAGER_OPERATION_WAV)
    {
        memcpy(command.wav_path,
               s_runtime.playback_resume.wav_path,
               strnlen(s_runtime.playback_resume.wav_path,
                       AUDIO_MANAGER_WAV_PATH_MAX_BYTES) + 1U);
    }

    if (xQueueSend(s_runtime.command_queue, &command, 0U) != pdTRUE)
    {
        portENTER_CRITICAL(&s_control_lock);
        s_control.operation = AUDIO_MANAGER_OPERATION_NONE;
        s_control.playback_status = previous;
        s_control.playback_status.last_control_result = ESP_ERR_TIMEOUT;
        portEXIT_CRITICAL(&s_control_lock);
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_runtime.status_mutex);
    return ESP_OK;
}

esp_err_t audio_manager_stop_playback(void)
{
    if (!s_runtime.initialized || (s_runtime.status_mutex == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_status_mutex("cancelling playback"))
    {
        return ESP_ERR_TIMEOUT;
    }

    bool requested = false;
    bool discard_retained = false;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.task_running &&
        !s_control.shutdown_requested &&
        ((s_control.operation == AUDIO_MANAGER_OPERATION_WAV) ||
         (s_control.operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK) ||
         (s_control.operation == AUDIO_MANAGER_OPERATION_PCM16_STREAM)))
    {
        s_control.cancel_requested = true;
        s_control.pause_requested = false;
        s_control.resume_requested = false;
        s_control.restart_requested = false;
        audio_manager_playback_control_state_t next =
            s_control.playback_status.state;
        if (audio_manager_playback_control_transition(
                s_control.playback_status.state,
                AUDIO_MANAGER_PLAYBACK_EVENT_STOP_REQUESTED,
                &next) == ESP_OK)
        {
            s_control.playback_status.state = next;
        }
        requested = true;
        result = ESP_OK;
    }
    else if (s_control.task_running &&
             !s_control.shutdown_requested &&
             (s_control.operation == AUDIO_MANAGER_OPERATION_NONE) &&
             s_runtime.playback_resume.valid &&
             (s_control.playback_status.state ==
              AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED))
    {
        audio_manager_playback_control_state_t next =
            s_control.playback_status.state;
        if (audio_manager_playback_control_transition(
                s_control.playback_status.state,
                AUDIO_MANAGER_PLAYBACK_EVENT_STOP_REQUESTED,
                &next) == ESP_OK)
        {
            s_control.playback_status.state = next;
            discard_retained = true;
            requested = true;
            result = ESP_OK;
        }
    }
    else if (s_control.task_running &&
             !s_control.shutdown_requested &&
             (s_control.operation == AUDIO_MANAGER_OPERATION_NONE) &&
             (s_control.playback_status.state ==
              AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE))
    {
        /* STOP is intentionally idempotent after cleanup has reached IDLE. */
        requested = true;
        result = ESP_OK;
    }
    s_control.playback_status.last_action =
        AUDIO_MANAGER_PLAYBACK_ACTION_STOP;
    s_control.playback_status.last_control_result = result;
    portEXIT_CRITICAL(&s_control_lock);

    if (discard_retained)
    {
        audio_manager_clear_resume_context();
        audio_manager_complete_playback_control(ESP_OK);
    }

    xSemaphoreGive(s_runtime.status_mutex);
    return requested ? ESP_OK : result;
}

esp_err_t audio_manager_pause_playback(
    audio_manager_playback_pause_reason_t reason)
{
    if ((reason != AUDIO_MANAGER_PLAYBACK_PAUSE_USER) &&
        (reason != AUDIO_MANAGER_PLAYBACK_PAUSE_PTT))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime.initialized || (s_runtime.status_mutex == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_manager_take_status_mutex("pausing playback"))
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_control_lock);
    const bool playback_operation =
        (s_control.operation == AUDIO_MANAGER_OPERATION_WAV) ||
        (s_control.operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK) ||
        (s_control.operation == AUDIO_MANAGER_OPERATION_PCM16_STREAM);
    if (s_control.task_running && !s_control.shutdown_requested &&
        playback_operation)
    {
        if (!s_control.playback_status.resumable)
        {
            result = ESP_ERR_NOT_SUPPORTED;
        }
        else
        {
            audio_manager_playback_control_state_t next =
                s_control.playback_status.state;
            result = audio_manager_playback_control_transition(
                s_control.playback_status.state,
                AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_REQUESTED,
                &next);
            if (result == ESP_OK)
            {
                s_control.pause_requested =
                    (next != AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED);
                s_control.requested_pause_reason = reason;
                s_control.playback_status.state = next;
                if (next == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED)
                {
                    s_control.playback_status.pause_reason = reason;
                }
            }
        }
    }
    else if (s_control.task_running && !s_control.shutdown_requested &&
             (s_control.operation == AUDIO_MANAGER_OPERATION_NONE) &&
             s_runtime.playback_resume.valid &&
             (s_control.playback_status.state ==
              AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED))
    {
        /* Idempotent pause of a released local source changes only owner
         * intent. USER must be able to replace a temporary PTT reason so a
         * later unrelated turn never resumes it accidentally. */
        s_control.playback_status.pause_reason = reason;
        s_runtime.playback_resume.pause_reason = reason;
        result = ESP_OK;
    }
    s_control.playback_status.last_action =
        AUDIO_MANAGER_PLAYBACK_ACTION_PAUSE;
    s_control.playback_status.last_control_result = result;
    portEXIT_CRITICAL(&s_control_lock);
    xSemaphoreGive(s_runtime.status_mutex);
    return result;
}

esp_err_t audio_manager_resume_playback(uint32_t expected_generation)
{
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_manager_playback_status_t snapshot = {0};
    if (audio_manager_get_playback_status(&snapshot) == ESP_OK &&
        (snapshot.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED))
    {
        /* PAUSED becomes visible after the retained source has released I2S,
         * but before its worker clears operation. Queueing in that hand-off
         * window returns INVALID_STATE; latch the request for the owner to
         * consume instead of losing a valid rapid resume. */
        bool handoff_pending = false;
        portENTER_CRITICAL(&s_control_lock);
        const bool local_operation =
            (s_control.operation == AUDIO_MANAGER_OPERATION_WAV) ||
            (s_control.operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK);
        if (s_control.task_running && !s_control.shutdown_requested &&
            local_operation &&
            (s_control.playback_status.state ==
             AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED) &&
            ((expected_generation == 0U) ||
             (expected_generation == s_control.playback_status.generation)))
        {
            s_control.resume_requested = true;
            s_control.playback_status.last_action =
                AUDIO_MANAGER_PLAYBACK_ACTION_RESUME;
            s_control.playback_status.last_control_result = ESP_OK;
            handoff_pending = true;
        }
        portEXIT_CRITICAL(&s_control_lock);
        if (handoff_pending)
        {
            return ESP_OK;
        }
        return audio_manager_queue_retained_playback(
            expected_generation,
            false);
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_control_lock);
    const bool local_operation =
        (s_control.operation == AUDIO_MANAGER_OPERATION_WAV) ||
        (s_control.operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK);
    if (s_control.task_running && !s_control.shutdown_requested)
    {
        if ((expected_generation != 0U) &&
            (expected_generation != s_control.playback_status.generation))
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else if (s_control.operation == AUDIO_MANAGER_OPERATION_PCM16_STREAM)
        {
            result = ESP_ERR_NOT_SUPPORTED;
        }
        else if (local_operation &&
                 (s_control.playback_status.state ==
                  AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING))
        {
            /* The owner still reaches PAUSED before applying this rapid resume. */
            s_control.resume_requested = true;
            result = ESP_OK;
        }
    }
    s_control.playback_status.last_action =
        AUDIO_MANAGER_PLAYBACK_ACTION_RESUME;
    s_control.playback_status.last_control_result = result;
    portEXIT_CRITICAL(&s_control_lock);
    return result;
}

esp_err_t audio_manager_restart_playback(void)
{
    return audio_manager_restart_playback_at_generation(0U);
}

esp_err_t audio_manager_restart_playback_at_generation(
    uint32_t expected_generation)
{
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_manager_playback_status_t snapshot = {0};
    if (audio_manager_get_playback_status(&snapshot) == ESP_OK &&
        (snapshot.state == AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED))
    {
        return audio_manager_queue_retained_playback(
            expected_generation,
            true);
    }

    esp_err_t result = ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_control_lock);
    const bool local_operation =
        (s_control.operation == AUDIO_MANAGER_OPERATION_WAV) ||
        (s_control.operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK);
    if (s_control.task_running && !s_control.shutdown_requested)
    {
        if ((expected_generation != 0U) &&
            (expected_generation != s_control.playback_status.generation))
        {
            result = ESP_ERR_INVALID_STATE;
        }
        else if (s_control.operation == AUDIO_MANAGER_OPERATION_PCM16_STREAM)
        {
            result = ESP_ERR_NOT_SUPPORTED;
        }
        else if (local_operation && s_control.playback_status.resumable)
        {
            audio_manager_playback_control_state_t next =
                s_control.playback_status.state;
            result = audio_manager_playback_control_transition(
                s_control.playback_status.state,
                AUDIO_MANAGER_PLAYBACK_EVENT_RESTART_REQUESTED,
                &next);
            if (result == ESP_OK)
            {
                s_control.restart_requested = true;
                s_control.pause_requested = false;
                s_control.resume_requested = false;
                s_control.playback_status.state = next;
                s_control.playback_status.pause_reason =
                    AUDIO_MANAGER_PLAYBACK_PAUSE_NONE;
                s_control.playback_status.position_frames = 0U;
            }
        }
    }
    s_control.playback_status.last_action =
        AUDIO_MANAGER_PLAYBACK_ACTION_RESTART;
    s_control.playback_status.last_control_result = result;
    portEXIT_CRITICAL(&s_control_lock);
    return result;
}

esp_err_t audio_manager_get_playback_status(
    audio_manager_playback_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_control_lock);
    *status = s_control.playback_status;
    portEXIT_CRITICAL(&s_control_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stop(void)
{
    if (!s_runtime.initialized ||
        (s_runtime.status_mutex == NULL) ||
        (s_runtime.command_queue == NULL) ||
        (s_runtime.lifecycle_events == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    const TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    if (current_task == s_runtime.task_handle)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_status_mutex("stopping manager task"))
    {
        return ESP_ERR_TIMEOUT;
    }

    bool task_running;
    bool first_request = false;
    portENTER_CRITICAL(&s_control_lock);
    task_running = s_control.task_running;
    if (task_running)
    {
        first_request = !s_control.shutdown_requested;
        s_control.shutdown_requested = true;
        s_control.cancel_requested = true;
        s_control.record_stop_requested = true;
    }
    portEXIT_CRITICAL(&s_control_lock);

    if (!task_running)
    {
        const esp_err_t result = s_runtime.task_exit_result;
        xSemaphoreGive(s_runtime.status_mutex);
        return result;
    }

    if (first_request)
    {
        const audio_manager_command_t command = {
            .kind = AUDIO_MANAGER_COMMAND_SHUTDOWN,
        };
        if (xQueueSend(s_runtime.command_queue, &command, 0U) != pdTRUE)
        {
            APP_LOGW(
                TAG, SHUTDOWN_COMMAND_QUEUE_FULL_31C1BE66,
                "Shutdown command queue full; polling fallback active");
        }
    }

    xSemaphoreGive(s_runtime.status_mutex);

    const EventBits_t expected_stopped_bits = AUDIO_MANAGER_TASK_STOPPED_BIT;

    const EventBits_t stopped_bits = xEventGroupWaitBits(
        s_runtime.lifecycle_events,
        expected_stopped_bits,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(AUDIO_MANAGER_TASK_STOP_TIMEOUT_MS));
    if ((stopped_bits & expected_stopped_bits) != expected_stopped_bits)
    {
        APP_LOGE(
            TAG, AUDIO_TASK_STOP_TIMED_OUT_2FC4F4DF,
            "Audio task stop timed out; shutdown remains pending");
        return ESP_ERR_TIMEOUT;
    }

    return s_runtime.task_exit_result;
}

esp_err_t audio_manager_get_status(audio_manager_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_runtime.initialized || (s_runtime.status_mutex == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_runtime.status_mutex,
            pdMS_TO_TICKS(AUDIO_MANAGER_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    audio_manager_refresh_diagnostics_locked();
    *status = s_runtime.status;

    xSemaphoreGive(s_runtime.status_mutex);
    return ESP_OK;
}

esp_err_t audio_manager_deinit(void)
{
    if (!s_runtime.initialized)
    {
        return ESP_OK;
    }

    bool task_running;
    portENTER_CRITICAL(&s_control_lock);
    task_running = s_control.task_running;
    portEXIT_CRITICAL(&s_control_lock);

    if (task_running ||
        (s_runtime.task_handle != NULL) ||
        audio_wav_prefetch_is_active(
            &s_runtime.playback_source.wav_prefetch))
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = force_cycle_cleanup();

    heap_caps_free(s_runtime.recording_pcm24);
    heap_caps_free(s_runtime.dsp_workspace);
    heap_caps_free(s_runtime.pcm_stream_storage);
    s_runtime.recording_pcm24 = NULL;
    s_runtime.dsp_workspace = NULL;
    s_runtime.pcm_stream_storage = NULL;

    if (s_runtime.command_queue != NULL)
    {
        vQueueDelete(s_runtime.command_queue);
        s_runtime.command_queue = NULL;
    }

    if (s_runtime.lifecycle_events != NULL)
    {
        vEventGroupDelete(s_runtime.lifecycle_events);
        s_runtime.lifecycle_events = NULL;
    }

    if (s_runtime.status_mutex != NULL)
    {
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.status_mutex = NULL;
    }

    if (s_runtime.pcm_stream_mutex != NULL)
    {
        vSemaphoreDelete(s_runtime.pcm_stream_mutex);
        s_runtime.pcm_stream_mutex = NULL;
    }

    audio_manager_reset_control();
    memset(&s_runtime, 0, sizeof(s_runtime));
    return result;
}

const char *audio_manager_state_to_string(audio_manager_state_t state)
{
    switch (state)
    {
        case AUDIO_MANAGER_STATE_UNINITIALIZED:
            return "UNINITIALIZED";

        case AUDIO_MANAGER_STATE_INITIALIZED:
            return "INITIALIZED";

        case AUDIO_MANAGER_STATE_IDLE:
            return "IDLE";

        case AUDIO_MANAGER_STATE_RECORDING:
            return "RECORDING";

        case AUDIO_MANAGER_STATE_PROCESSING:
            return "PROCESSING";

        case AUDIO_MANAGER_STATE_PLAYBACK:
            return "PLAYBACK";

        case AUDIO_MANAGER_STATE_ERROR:
            return "ERROR";

        default:
            return "UNKNOWN";
    }
}

const char *audio_manager_playback_control_state_to_string(
    audio_manager_playback_control_state_t state)
{
    switch (state)
    {
        case AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE: return "IDLE";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING: return "STARTING";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING: return "PLAYING";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING: return "PAUSING";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED: return "PAUSED";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING: return "RESUMING";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_STOPPING: return "STOPPING";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char *audio_manager_playback_source_to_string(
    audio_manager_playback_source_t source)
{
    switch (source)
    {
        case AUDIO_MANAGER_PLAYBACK_SOURCE_NONE: return "NONE";
        case AUDIO_MANAGER_PLAYBACK_SOURCE_RECORDED: return "RECORDED";
        case AUDIO_MANAGER_PLAYBACK_SOURCE_WAV: return "WAV";
        case AUDIO_MANAGER_PLAYBACK_SOURCE_PCM16_STREAM: return "PCM16_STREAM";
        default: return "UNKNOWN";
    }
}
