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
#define AUDIO_MANAGER_WAV_PREFETCH_BYTES_PER_SECOND \
    (AUDIO_MANAGER_SAMPLE_RATE_HZ * sizeof(int16_t))
#define AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES \
    (AUDIO_MANAGER_WAV_PREFETCH_BYTES_PER_SECOND * \
     CONFIG_AUDIO_MANAGER_WAV_PREFETCH_SECONDS)
#define AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS \
    CONFIG_AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS

#ifdef CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP
#define AUDIO_MANAGER_WAV_STRESS_TASK_NAME             "wav_stress"
#define AUDIO_MANAGER_WAV_STRESS_TASK_STACK_SIZE       3072U
#define AUDIO_MANAGER_WAV_STRESS_RETRY_DELAY_MS         100U
#define AUDIO_MANAGER_WAV_STRESS_POST_COMPLETION_DELAY_MS \
    (CONFIG_AUDIO_MANAGER_WAV_STRESS_POST_COMPLETION_DELAY_SECONDS * 1000U)
#define AUDIO_MANAGER_WAV_STRESS_TASK_PRIORITY \
    CONFIG_AUDIO_MANAGER_WAV_STRESS_TASK_PRIORITY
#endif

#define AUDIO_MANAGER_TASK_READY_BIT  ((EventBits_t)(1U << 0U))
#define AUDIO_MANAGER_TASK_STOPPED_BIT ((EventBits_t)(1U << 1U))
#define AUDIO_MANAGER_WAV_STRESS_TASK_STOPPED_BIT ((EventBits_t)(1U << 2U))
#define AUDIO_MANAGER_WAV_STRESS_TASK_SHUTDOWN_BIT ((EventBits_t)(1U << 3U))

/* Production remains lightweight; test modes can raise the I2S owner to 6/7. */
#ifdef CONFIG_AUDIO_MANAGER_GOLDEN_STABILITY_MODE
#define AUDIO_MANAGER_DEFAULT_RECORD_SECONDS \
    CONFIG_AUDIO_MANAGER_GOLDEN_STABILITY_RECORD_SECONDS
#define AUDIO_MANAGER_TASK_PRIORITY \
    CONFIG_AUDIO_MANAGER_GOLDEN_STABILITY_TASK_PRIORITY
#elif defined(CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP)
#define AUDIO_MANAGER_DEFAULT_RECORD_SECONDS          5U
#define AUDIO_MANAGER_TASK_PRIORITY \
    CONFIG_AUDIO_MANAGER_WAV_STRESS_TASK_PRIORITY
#else
#define AUDIO_MANAGER_DEFAULT_RECORD_SECONDS          5U
#define AUDIO_MANAGER_TASK_PRIORITY                   7U
#endif

/* Proven transport baseline. Do not change during structural refactor. */
#define AUDIO_MANAGER_SAMPLE_RATE_HZ                  AUDIO_DSP_SAMPLE_RATE_HZ
#define AUDIO_MANAGER_SLOT_COUNT                      2U
#define AUDIO_MANAGER_FRAMES_PER_BLOCK                256U
#define AUDIO_MANAGER_DMA_DESC_NUM                    8U
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
} audio_playback_source_kind_t;

typedef struct
{
    audio_playback_source_kind_t kind;
    size_t recorded_sample_count;
    audio_wav_prefetch_t wav_prefetch;
} audio_playback_source_t;

typedef enum
{
    AUDIO_MANAGER_COMMAND_RECORD_FIXED = 0,
    AUDIO_MANAGER_COMMAND_RECORD_MANUAL,
    AUDIO_MANAGER_COMMAND_PLAY_RECORDED,
    AUDIO_MANAGER_COMMAND_PLAY_WAV,
    AUDIO_MANAGER_COMMAND_SHUTDOWN,
} audio_manager_command_kind_t;

typedef struct
{
    audio_manager_command_kind_t kind;
    char wav_path[AUDIO_MANAGER_WAV_PATH_MAX_BYTES];
} audio_manager_command_t;

typedef enum
{
    AUDIO_MANAGER_OPERATION_NONE = 0,
    AUDIO_MANAGER_OPERATION_RECORD_FIXED,
    AUDIO_MANAGER_OPERATION_RECORD_MANUAL,
    AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK,
    AUDIO_MANAGER_OPERATION_WAV,
    AUDIO_MANAGER_OPERATION_STABILITY,
} audio_manager_operation_t;

/** @brief Completion record used only by the continuous WAV-stress coordinator. */
typedef struct
{
    uint32_t sequence;
    esp_err_t result;
    bool cancelled;
} audio_manager_wav_completion_t;

typedef struct
{
    bool task_running;
    bool shutdown_requested;
    bool cancel_requested;
    bool record_stop_requested;
    audio_manager_operation_t operation;
    audio_manager_wav_completion_t wav_completion;
} audio_manager_control_t;

typedef struct
{
    bool initialized;
    bool rx_enabled;
    bool tx_enabled;

    audio_manager_config_t config;

    /* One manager-owned source slot; only this task/lifecycle owns it. */
    audio_playback_source_t playback_source;

    size_t sample_capacity;
    size_t fixed_record_sample_count;
    size_t manual_record_sample_limit;
    size_t recording_bytes;
    int32_t *recording_pcm24;
    audio_dsp_workspace_t *dsp_workspace;

    bool recorded_audio_valid;
    size_t recorded_sample_count;

    i2s_chan_handle_t rx_channel;
    i2s_chan_handle_t tx_channel;

    TaskHandle_t task_handle;
    TaskHandle_t wav_stress_task_handle;
    SemaphoreHandle_t status_mutex;
    QueueHandle_t command_queue;
    EventGroupHandle_t lifecycle_events;
    esp_err_t task_exit_result;

    audio_manager_status_t status;
    audio_manager_status_callback_t status_callback;
    void *status_callback_context;

    /* Set by stop() so the optional coordinator can leave its 60-second wait. */
    bool wav_stress_shutdown_requested;
} audio_manager_runtime_t;

/* Static Variables --------------------------------------------------------- */
static audio_manager_runtime_t s_runtime = {0};

/* Small I2S staging stays in Internal/DMA RAM; long history stays in PSRAM. */
DMA_ATTR static int32_t s_rx_block[
    AUDIO_MANAGER_FRAMES_PER_BLOCK * AUDIO_MANAGER_SLOT_COUNT];
DMA_ATTR static int16_t s_tx_block[
    AUDIO_MANAGER_FRAMES_PER_BLOCK * AUDIO_MANAGER_SLOT_COUNT];
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
static void audio_manager_reset_diagnostics(void);
static void audio_manager_snapshot_diagnostics(
    audio_manager_diagnostics_t *diagnostics);
static void audio_manager_set_capture_i2s_active(bool active);
static void audio_manager_set_playback_i2s_active(bool active);
static void audio_manager_reset_control(void);
static bool audio_manager_cancel_is_requested(void);
static bool audio_manager_shutdown_is_requested(void);
static audio_record_stop_reason_t audio_manager_record_stop_reason(
    audio_record_control_t control);
static bool audio_manager_recorded_playback_cancel_enabled(void);
static void audio_manager_finish_operation(void);
#ifdef CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP
static audio_manager_wav_completion_t
audio_manager_snapshot_wav_completion(void);
#endif
static void audio_manager_finish_wav_operation(
    esp_err_t result,
    bool cancelled);
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
static int32_t apply_playback_volume_percent(int32_t sample_pcm24);
static int16_t decode_wav_pcm16_le(const uint8_t *sample_bytes);
static int16_t apply_wav_volume_percent(int16_t sample_pcm16);
static esp_err_t play_recording(
    size_t sample_count,
    audio_dsp_playback_stats_t *stats,
    bool *cancelled);
static esp_err_t play_wav_stream(
    audio_wav_prefetch_t *prefetch,
    audio_wav_playback_metrics_t *metrics,
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
static esp_err_t audio_manager_select_wav_playback_source(const char *path);
static esp_err_t audio_manager_release_playback_source(void);
static esp_err_t force_cycle_cleanup(void);
static esp_err_t run_cycle(audio_cycle_metrics_t *metrics);
static void audio_manager_handle_record_command(bool manual);
static void audio_manager_handle_recorded_playback_command(void);
static void audio_manager_handle_wav_command(const char *path);
static void audio_manager_run_stability_iteration(void);
static bool audio_manager_stability_mode_enabled(void);
static bool audio_manager_mixed_stress_mode_enabled(void);
static bool audio_manager_wav_stress_mode_enabled(void);
static const char *audio_manager_wav_regression_path(void);
static esp_err_t audio_manager_queue_simple_operation(
    audio_manager_command_kind_t command_kind,
    audio_manager_operation_t operation,
    bool require_recorded_audio,
    const char *description);
static void log_cycle_diagnostics(
    uint32_t cycle,
    const audio_manager_diagnostics_t *before);
static void audio_manager_task(void *argument);

#ifdef CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP
static bool audio_manager_wav_stress_shutdown_is_requested(void);
static void audio_manager_wav_stress_wait(uint32_t delay_ms);
static void audio_manager_wav_stress_task(void *argument);
#endif

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

    ESP_LOGE(
        TAG,
        "Status mutex timeout while %s",
        (operation != NULL) ? operation : "updating diagnostics");
    return false;
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
    s_control = (audio_manager_control_t) {0};
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
    portEXIT_CRITICAL(&s_control_lock);
}

#ifdef CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP
static audio_manager_wav_completion_t
audio_manager_snapshot_wav_completion(void)
{
    audio_manager_wav_completion_t completion;

    portENTER_CRITICAL(&s_control_lock);
    completion = s_control.wav_completion;
    portEXIT_CRITICAL(&s_control_lock);

    return completion;
}
#endif

/**
 * @brief Publish one terminal WAV result even when GUI-status storage timed out.
 *
 * The coordinator uses this private sequence instead of status counters so a
 * bounded status-mutex timeout cannot stall an otherwise completed stress run.
 */
static void audio_manager_finish_wav_operation(
    esp_err_t result,
    bool cancelled)
{
    portENTER_CRITICAL(&s_control_lock);
    s_control.operation = AUDIO_MANAGER_OPERATION_NONE;
    s_control.cancel_requested = false;
    s_control.record_stop_requested = false;
    ++s_control.wav_completion.sequence;
    s_control.wav_completion.result = result;
    s_control.wav_completion.cancelled = cancelled;
    portEXIT_CRITICAL(&s_control_lock);
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
        ESP_LOGD(TAG, "State -> %s", audio_manager_state_to_string(state));
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

    ESP_LOGI(
        TAG,
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
        ESP_LOGW(
            TAG,
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
                audio_dsp_convert_raw_slot_to_pcm24(s_rx_block[base]));
            update_slot_stats(
                &right,
                audio_dsp_convert_raw_slot_to_pcm24(s_rx_block[base + 1U]));
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

    ESP_LOGI(
        TAG,
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
                audio_dsp_convert_raw_slot_to_pcm24(s_rx_block[source_index]);
        }

        captured += frames_read;

        while ((captured >= next_progress) &&
               (next_progress <= target_sample_count))
        {
            ESP_LOGI(
                TAG,
                "Recorded %u/%u seconds",
                (unsigned)(next_progress / AUDIO_MANAGER_SAMPLE_RATE_HZ),
                (unsigned)(target_sample_count / AUDIO_MANAGER_SAMPLE_RATE_HZ));
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
    size_t sample_count,
    audio_dsp_playback_stats_t *stats,
    bool *cancelled)
{
    if ((s_runtime.recording_pcm24 == NULL) ||
        (sample_count == 0U) ||
        (stats == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(stats, 0, sizeof(*stats));
    size_t sample_index = 0U;

    while (sample_index < sample_count)
    {
        if ((cancelled != NULL) && audio_manager_cancel_is_requested())
        {
            *cancelled = true;
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
    bool *cancelled)
{
    if ((prefetch == NULL) || (item == NULL) || (metrics == NULL) ||
        (cancelled == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t wait_start_us = esp_timer_get_time();
    while (true)
    {
        if (audio_manager_cancel_is_requested())
        {
            *cancelled = true;
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
            ESP_LOGE(
                TAG,
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
    bool *cancelled)
{
    if ((prefetch == NULL) || (item == NULL) || (metrics == NULL) ||
        (cancelled == NULL))
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
        if (audio_manager_cancel_is_requested())
        {
            *cancelled = true;
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
    }

    return ESP_OK;
}

static esp_err_t play_wav_stream(
    audio_wav_prefetch_t *prefetch,
    audio_wav_playback_metrics_t *metrics,
    bool *cancelled)
{
    if ((prefetch == NULL) || (metrics == NULL) || (cancelled == NULL) ||
        !audio_wav_prefetch_is_active(prefetch))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(metrics, 0, sizeof(*metrics));
    metrics->fixed_scale_gain_q16 =
        AUDIO_WAV_PCM16_FULL_SCALE_GAIN_Q16(
            AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
    *cancelled = false;
    metrics->prefetch_block_bytes = AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES;

    audio_wav_prefetch_item_t item = {0};
    bool item_held = false;
    esp_err_t result = audio_manager_take_prefetched_wav_item(
        prefetch,
        &item,
        metrics,
        true,
        cancelled);
    if ((result != ESP_OK) || *cancelled)
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
    }

    if ((result == ESP_OK) && audio_manager_cancel_is_requested())
    {
        *cancelled = true;
    }

    bool tx_started = false;
    if ((result == ESP_OK) && !*cancelled)
    {
        result = start_i2s_tx();
        tx_started = (result == ESP_OK);
    }

    if ((result == ESP_OK) && !*cancelled)
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS,
            cancelled);
    }

    int64_t playback_start_us = 0;
    if ((result == ESP_OK) && !*cancelled)
    {
        playback_start_us = esp_timer_get_time();
        audio_manager_set_state(AUDIO_MANAGER_STATE_PLAYBACK);
    }

    while ((result == ESP_OK) && !*cancelled && item_held)
    {
        result = audio_manager_play_prefetched_wav_item(
            prefetch,
            &item,
            metrics,
            cancelled);

        const bool final_block = item.final_block;
        const esp_err_t release_result = audio_wav_prefetch_release_slot(
            prefetch,
            item.slot_index);
        item_held = false;
        if ((result == ESP_OK) && (release_result != ESP_OK))
        {
            result = release_result;
        }

        if ((result != ESP_OK) || *cancelled)
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
            cancelled);
        item_held = (result == ESP_OK) && !*cancelled;
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

    if ((result == ESP_OK) && !*cancelled &&
        (metrics->data_bytes_streamed != metrics->expected_data_bytes))
    {
        result = ESP_ERR_INVALID_SIZE;
    }

    if ((result == ESP_OK) && !*cancelled)
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_POST_PLAYBACK_SILENCE_BLOCKS,
            cancelled);
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
            ESP_LOGW(TAG, "Waiting for WAV prefetch reader to drain");
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

    ESP_LOGI(
        TAG,
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

    ESP_LOGI(
        TAG,
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
        ESP_LOGI(
            TAG,
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

    ESP_LOGI(
        TAG,
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
            ESP_LOGI(
                TAG,
                "WAV prefetch playback block=%uB cache=%us x%u volume=%u/100 policy=fixed_full_scale_pcm16 fixed_gain_q16=%u ceiling=+/-%u",
                (unsigned)AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES,
                (unsigned)CONFIG_AUDIO_MANAGER_WAV_PREFETCH_SECONDS,
                (unsigned)AUDIO_WAV_PREFETCH_SLOT_COUNT,
                (unsigned)s_runtime.config.playback_volume_percent,
                (unsigned)AUDIO_WAV_PCM16_FULL_SCALE_GAIN_Q16(
                    AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16),
                (unsigned)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
            return play_wav_stream(
                &source->wav_prefetch,
                &metrics->wav,
                cancelled);

        default:
            return ESP_ERR_INVALID_ARG;
    }

    const bool cancellable = audio_manager_recorded_playback_cancel_enabled();
    bool *const cancellation = cancellable ? cancelled : NULL;

    esp_err_t result = start_i2s_tx();
    if (result != ESP_OK)
    {
        return result;
    }

    result = write_silence_blocks(
        AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS,
        cancellation);

    if ((result == ESP_OK) && !*cancelled)
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_PLAYBACK);

        ESP_LOGI(
            TAG,
            "PLAYBACK samples=%u volume=%u/100 gain=%u.%02ux limiter=+/-%u",
            (unsigned)source->recorded_sample_count,
            (unsigned)s_runtime.config.playback_volume_percent,
            (unsigned)(AUDIO_DSP_PLAYBACK_GAIN_Q8 / AUDIO_DSP_Q8_ONE),
            (unsigned)(((AUDIO_DSP_PLAYBACK_GAIN_Q8 %
                          AUDIO_DSP_Q8_ONE) * 100U) /
                        AUDIO_DSP_Q8_ONE),
            (unsigned)AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16);
        result = play_recording(
            source->recorded_sample_count,
            &metrics->playback,
            cancellation);
    }

    if ((result == ESP_OK) && !*cancelled)
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_POST_PLAYBACK_SILENCE_BLOCKS,
            cancellation);
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

static esp_err_t audio_manager_select_wav_playback_source(const char *path)
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

    const esp_err_t result = audio_wav_prefetch_start(
        &s_runtime.playback_source.wav_prefetch,
        path,
        AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES,
        AUDIO_MANAGER_WAV_PREFETCH_READER_PRIORITY);
    if (result != ESP_OK)
    {
        return result;
    }

    s_runtime.playback_source.kind = AUDIO_PLAYBACK_SOURCE_WAV_PCM16;
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
                    ESP_LOGW(TAG, "Waiting to release WAV prefetch reader");
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
        ESP_LOGE(
            TAG,
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
#ifdef CONFIG_AUDIO_MANAGER_GOLDEN_STABILITY_MODE
    return true;
#else
    return false;
#endif
}

static bool audio_manager_mixed_stress_mode_enabled(void)
{
#if defined(CONFIG_AUDIO_MANAGER_GOLDEN_STABILITY_MODE) && \
    defined(CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP)
    return true;
#else
    return false;
#endif
}

static bool audio_manager_wav_stress_mode_enabled(void)
{
#ifdef CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP
    return true;
#else
    return false;
#endif
}

static const char *audio_manager_wav_regression_path(void)
{
#ifdef CONFIG_AUDIO_MANAGER_WAV_VALIDATION_ONCE
    return CONFIG_AUDIO_MANAGER_WAV_VALIDATION_PATH;
#else
    return NULL;
#endif
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
        (s_runtime.status.state == AUDIO_MANAGER_STATE_IDLE))
    {
        s_control.operation = operation;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
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
        audio_manager_finish_operation();
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(s_runtime.status_mutex);
    return ESP_OK;
}

#ifdef CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP
static bool audio_manager_wav_stress_shutdown_is_requested(void)
{
    bool shutdown_requested;

    portENTER_CRITICAL(&s_control_lock);
    shutdown_requested = s_runtime.wav_stress_shutdown_requested;
    portEXIT_CRITICAL(&s_control_lock);

    return shutdown_requested;
}

/**
 * @brief Wait without delaying lifecycle shutdown for the optional test task.
 *
 * audio_manager_stop() sets a lifecycle event bit so this task can leave
 * either its short retry wait or configured post-completion sleep immediately.
 */
static void audio_manager_wav_stress_wait(uint32_t delay_ms)
{
    if (s_runtime.lifecycle_events != NULL)
    {
        (void)xEventGroupWaitBits(
            s_runtime.lifecycle_events,
            AUDIO_MANAGER_WAV_STRESS_TASK_SHUTDOWN_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(delay_ms));
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief Submit the configured WAV only through the manager command API.
 *
 * This is a test coordinator, not a second I2S or SD/VFS owner. It submits
 * through the normal command-idle path, or through the bounded window between
 * complete golden record/DSP/recorded-playback cycles when golden is enabled.
 */
static void audio_manager_wav_stress_task(void *argument)
{
    (void)argument;

    // const char *const path = CONFIG_AUDIO_MANAGER_WAV_STRESS_PATH;
    // const char *const path = "/sdcard/audio/input_long.wav";
    const char *const path = "/sdcard/audio/input_2.wav";
    bool waiting_for_sd = false;
    uint32_t iteration = 0U;

    ESP_LOGI(
        TAG,
        "WAV stress coordinator started: path=%s priority=%u delay=%us",
        path,
        (unsigned)AUDIO_MANAGER_WAV_STRESS_TASK_PRIORITY,
        (unsigned)CONFIG_AUDIO_MANAGER_WAV_STRESS_POST_COMPLETION_DELAY_SECONDS);

    while (!audio_manager_wav_stress_shutdown_is_requested())
    {
        if (!sd_card_manager_is_mounted())
        {
            if (!waiting_for_sd)
            {
                ESP_LOGI(TAG, "WAV stress waiting for SD VFS readiness");
                waiting_for_sd = true;
            }

            audio_manager_wav_stress_wait(
                AUDIO_MANAGER_WAV_STRESS_RETRY_DELAY_MS);
            continue;
        }

        if (waiting_for_sd)
        {
            ESP_LOGI(TAG, "WAV stress detected SD VFS readiness");
            waiting_for_sd = false;
        }

        audio_manager_status_t before = {0};
        const esp_err_t status_result = audio_manager_get_status(&before);
        if (status_result != ESP_OK)
        {
            audio_manager_wav_stress_wait(
                AUDIO_MANAGER_WAV_STRESS_RETRY_DELAY_MS);
            continue;
        }

        if ((before.state == AUDIO_MANAGER_STATE_INITIALIZED) ||
            (before.state == AUDIO_MANAGER_STATE_UNINITIALIZED))
        {
            audio_manager_wav_stress_wait(
                AUDIO_MANAGER_WAV_STRESS_RETRY_DELAY_MS);
            continue;
        }

        const audio_manager_wav_completion_t completion_before =
            audio_manager_snapshot_wav_completion();
        const esp_err_t request_result = audio_manager_play_wav(path);
        if (request_result == ESP_OK)
        {
            ++iteration;
            ESP_LOGI(
                TAG,
                "WAV_STRESS #%u accepted; waiting for terminal result",
                (unsigned)iteration);

            audio_manager_wav_completion_t completion_after =
                completion_before;
            bool terminal = false;
            while (!audio_manager_wav_stress_shutdown_is_requested() &&
                   !terminal)
            {
                completion_after = audio_manager_snapshot_wav_completion();
                terminal =
                    (completion_after.sequence != completion_before.sequence);

                if (!terminal)
                {
                    audio_manager_wav_stress_wait(
                        AUDIO_MANAGER_WAV_STRESS_RETRY_DELAY_MS);
                }
            }

            if (audio_manager_wav_stress_shutdown_is_requested())
            {
                break;
            }

            const char *const outcome =
                completion_after.cancelled
                    ? "cancelled"
                    : (completion_after.result == ESP_OK) ? "completed" : "failed";
            ESP_LOGI(
                TAG,
                "WAV_STRESS #%u %s: %s; sleeping %us",
                (unsigned)iteration,
                outcome,
                esp_err_to_name(completion_after.result),
                (unsigned)CONFIG_AUDIO_MANAGER_WAV_STRESS_POST_COMPLETION_DELAY_SECONDS);

            audio_manager_wav_stress_wait(
                AUDIO_MANAGER_WAV_STRESS_POST_COMPLETION_DELAY_MS);
            continue;
        }

        if ((request_result != ESP_ERR_INVALID_STATE) &&
            (request_result != ESP_ERR_TIMEOUT))
        {
            ESP_LOGE(
                TAG,
                "WAV stress request rejected: %s; retrying after %us",
                esp_err_to_name(request_result),
                (unsigned)CONFIG_AUDIO_MANAGER_WAV_STRESS_POST_COMPLETION_DELAY_SECONDS);
            audio_manager_wav_stress_wait(
                AUDIO_MANAGER_WAV_STRESS_POST_COMPLETION_DELAY_MS);
        }
        else
        {
            /* The manager owns I2S; retry when its command slot becomes free. */
            audio_manager_wav_stress_wait(
                AUDIO_MANAGER_WAV_STRESS_RETRY_DELAY_MS);
        }
    }

    /* stop() clears the handle only after it has observed this terminal bit. */
    xEventGroupSetBits(
        s_runtime.lifecycle_events,
        AUDIO_MANAGER_WAV_STRESS_TASK_STOPPED_BIT);

    ESP_LOGI(TAG, "WAV stress coordinator stopped");
    vTaskDelete(NULL);
}
#endif

static void audio_manager_handle_record_command(bool manual)
{
    ESP_LOGI(
        TAG,
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
            ESP_LOGW(
                TAG,
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
        ESP_LOGE(
            TAG,
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
        ESP_LOGI(
            TAG,
            "%s recording aborted by manager shutdown",
            manual ? "Manual" : "Fixed");
    }
    else if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
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
        ESP_LOGI(
            TAG,
            "%s recording completed: samples=%u manual_stop=%s",
            manual ? "Manual" : "Fixed",
            (unsigned)samples_recorded,
            manual_stopped ? "yes" : "no");
    }
    else
    {
        ESP_LOGI(
            TAG,
            "Manual recording ended without retained audio: samples=%u",
            (unsigned)samples_recorded);
    }
}

static void audio_manager_handle_recorded_playback_command(void)
{
    ESP_LOGI(TAG, "========== RECORDED PLAYBACK ==========");

    if (audio_manager_take_status_mutex("starting recorded playback"))
    {
        ++s_runtime.status.recorded_playback_started;
        xSemaphoreGive(s_runtime.status_mutex);
    }

    const size_t sample_count = s_runtime.recorded_sample_count;
    audio_cycle_metrics_t metrics = {0};
    bool cancelled = audio_manager_cancel_is_requested();
    esp_err_t result = ESP_OK;

    if (!s_runtime.recorded_audio_valid || (sample_count == 0U))
    {
        result = ESP_ERR_INVALID_STATE;
    }

    if ((result == ESP_OK) && !cancelled)
    {
        result = audio_manager_select_recording_playback_source(sample_count);
    }

    if ((result == ESP_OK) && !cancelled)
    {
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
        ESP_LOGE(
            TAG,
            "Recorded playback cleanup also failed: %s",
            esp_err_to_name(cleanup_result));
    }

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
        ESP_LOGE(
            TAG,
            "Recorded playback failed: %s",
            esp_err_to_name(result));
        if (status_updated)
        {
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        }
    }
    else if (cancelled)
    {
        ESP_LOGI(TAG, "Recorded playback cancelled");
    }
    else
    {
        ESP_LOGI(TAG, "Recorded playback completed");
    }
}

static void audio_manager_handle_wav_command(const char *path)
{
    ESP_LOGI(TAG, "========== WAV PLAYBACK ==========");
    ESP_LOGI(TAG, "WAV path=%s", path);

    if (audio_manager_take_status_mutex("starting WAV playback"))
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

    if (!cancelled)
    {
        result = audio_manager_select_wav_playback_source(path);
    }

    if (result == ESP_OK && !cancelled)
    {
        result = playback_once(
            &s_runtime.playback_source,
            &metrics,
            &cancelled);
    }

    if (audio_wav_prefetch_is_active(
            &s_runtime.playback_source.wav_prefetch))
    {
        const esp_err_t prefetch_result = audio_manager_copy_prefetch_metrics(
            &s_runtime.playback_source.wav_prefetch,
            &metrics.wav);
        if ((result == ESP_OK) && !cancelled &&
            (prefetch_result != ESP_OK))
        {
            result = prefetch_result;
        }
        else if ((result != ESP_OK) && (prefetch_result != ESP_OK) &&
                 (prefetch_result != result))
        {
            ESP_LOGE(
                TAG,
                "WAV prefetch reader also failed: %s",
                esp_err_to_name(prefetch_result));
        }
    }

    expected_data_bytes = metrics.wav.expected_data_bytes;
    expected_duration_ms = metrics.wav.expected_duration_ms;

    const esp_err_t cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (cleanup_result != ESP_OK))
    {
        result = cleanup_result;
        cancelled = false;
    }
    else if ((result != ESP_OK) && (cleanup_result != ESP_OK))
    {
        ESP_LOGE(
            TAG,
            "WAV cleanup also failed: %s",
            esp_err_to_name(cleanup_result));
    }

    audio_manager_diagnostics_t diagnostics_after = {0};
    audio_manager_snapshot_diagnostics(&diagnostics_after);

    ESP_LOGI(
        TAG,
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

    audio_manager_finish_wav_operation(result, cancelled);

    if (status_updated)
    {
        audio_manager_notify_status_changed();
    }

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "WAV playback failed: %s", esp_err_to_name(result));
        if (status_updated)
        {
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        }
    }
    else if (cancelled)
    {
        ESP_LOGI(TAG, "WAV playback cancelled");
    }
    else
    {
        ESP_LOGI(TAG, "WAV playback completed");
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

    ESP_LOGI(
        TAG,
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

    ESP_LOGI(
        TAG,
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
        ESP_LOGI(
            TAG,
            "CYCLE #%u PASS samples=%u",
            (unsigned)cycle,
            (unsigned)metrics.samples_recorded);
    }
    else
    {
        ESP_LOGE(
            TAG,
            "CYCLE #%u FAIL: %s",
            (unsigned)cycle,
            esp_err_to_name(result));

        /* Keep soak useful after transient faults: cleanup, report, retry. */
        const esp_err_t cleanup_result = force_cycle_cleanup();
        if (cleanup_result != ESP_OK)
        {
            ESP_LOGE(
                TAG,
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
    const bool wav_stress_mode = audio_manager_wav_stress_mode_enabled();
    ESP_LOGI(
        TAG,
        "Audio manager task started: mode=%s priority=%u volume=%u/100",
        stability_mode
            ? (mixed_stress_mode ? "golden_wav_stress" : "golden_stability")
            : (wav_stress_mode ? "wav_stress" : "production_idle"),
        (unsigned)AUDIO_MANAGER_TASK_PRIORITY,
        (unsigned)s_runtime.config.playback_volume_percent);

    audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    xEventGroupSetBits(
        s_runtime.lifecycle_events,
        AUDIO_MANAGER_TASK_READY_BIT);

    const char *const wav_regression_path =
        audio_manager_wav_regression_path();
    bool wav_regression_pending = (wav_regression_path != NULL);
    if (wav_regression_pending)
    {
        ESP_LOGI(
            TAG,
            "WAV startup regression is waiting for SD VFS readiness: %s",
            wav_regression_path);
    }

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
                        audio_manager_handle_wav_command(command.wav_path);
                    }
                    else if (command.kind == AUDIO_MANAGER_COMMAND_SHUTDOWN)
                    {
                        task_done = true;
                    }
                    else
                    {
                        ESP_LOGE(
                            TAG,
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
                    audio_manager_handle_wav_command(command.wav_path);
                }
                else if (command.kind == AUDIO_MANAGER_COMMAND_SHUTDOWN)
                {
                    task_done = true;
                }
                else
                {
                    ESP_LOGE(
                        TAG,
                        "Unexpected audio command in golden mode: %d",
                        (int)command.kind);
                }
            }
            continue;
        }

        /*
         * SD recovery is intentionally asynchronous at boot. Submit the
         * default-off hardware regression once only after the manager reports
         * VFS availability, rather than consuming its one command while the
         * card is still in the cold-start retry window. The stream itself
         * acquires the real lease immediately before fopen().
         */
        if (wav_regression_pending && sd_card_manager_is_mounted())
        {
            const esp_err_t regression_result =
                audio_manager_play_wav(wav_regression_path);
            if (regression_result == ESP_OK)
            {
                wav_regression_pending = false;
                ESP_LOGI(TAG, "WAV startup regression command accepted");
            }
            else if ((regression_result != ESP_ERR_INVALID_STATE) &&
                     (regression_result != ESP_ERR_TIMEOUT))
            {
                wav_regression_pending = false;
                ESP_LOGE(
                    TAG,
                    "WAV startup regression command failed permanently: %s",
                    esp_err_to_name(regression_result));
            }
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
                    audio_manager_handle_recorded_playback_command();
                    break;

                case AUDIO_MANAGER_COMMAND_PLAY_WAV:
                    audio_manager_handle_wav_command(command.wav_path);
                    break;

                case AUDIO_MANAGER_COMMAND_SHUTDOWN:
                    task_done = true;
                    break;

                default:
                    ESP_LOGE(
                        TAG,
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
        portENTER_CRITICAL(&s_control_lock);
        s_control = (audio_manager_control_t) {0};
        portEXIT_CRITICAL(&s_control_lock);
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

    ESP_LOGI(
        TAG,
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

    s_runtime.command_queue = xQueueCreate(
        AUDIO_MANAGER_COMMAND_QUEUE_LENGTH,
        sizeof(audio_manager_command_t));
    if (s_runtime.command_queue == NULL)
    {
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_runtime.lifecycle_events = xEventGroupCreate();
    if (s_runtime.lifecycle_events == NULL)
    {
        vQueueDelete(s_runtime.command_queue);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.command_queue = NULL;
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = hold_amplifier_data_low();
    if (result != ESP_OK)
    {
        vEventGroupDelete(s_runtime.lifecycle_events);
        vQueueDelete(s_runtime.command_queue);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.lifecycle_events = NULL;
        s_runtime.command_queue = NULL;
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

    if ((s_runtime.recording_pcm24 == NULL) ||
        (s_runtime.dsp_workspace == NULL) ||
        !esp_ptr_external_ram(s_runtime.recording_pcm24) ||
        !esp_ptr_external_ram(s_runtime.dsp_workspace))
    {
        heap_caps_free(s_runtime.recording_pcm24);
        heap_caps_free(s_runtime.dsp_workspace);
        s_runtime.recording_pcm24 = NULL;
        s_runtime.dsp_workspace = NULL;
        vEventGroupDelete(s_runtime.lifecycle_events);
        vQueueDelete(s_runtime.command_queue);
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.lifecycle_events = NULL;
        s_runtime.command_queue = NULL;
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(s_runtime.recording_pcm24, 0, s_runtime.recording_bytes);
    audio_dsp_workspace_init(s_runtime.dsp_workspace);

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

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Audio manager initialized");
    ESP_LOGI(
        TAG,
        "Pins BCLK=GPIO%d WS=GPIO%d MIC_DIN=GPIO%d SPK_DOUT=GPIO%d",
        AUDIO_GPIO_BCLK,
        AUDIO_GPIO_WS,
        AUDIO_GPIO_MIC_DIN,
        AUDIO_GPIO_SPK_DOUT);
    ESP_LOGI(
        TAG,
        "Config sample_rate=%u fixed_record=%us manual_max=%us capacity_samples=%u PCM24_PSRAM=%uB volume=%u/100 DMA=%ux%u",
        (unsigned)AUDIO_MANAGER_SAMPLE_RATE_HZ,
        (unsigned)config->record_duration_seconds,
        (unsigned)AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS,
        (unsigned)s_runtime.sample_capacity,
        (unsigned)s_runtime.recording_bytes,
        (unsigned)config->playback_volume_percent,
        (unsigned)AUDIO_MANAGER_DMA_DESC_NUM,
        (unsigned)AUDIO_MANAGER_FRAMES_PER_BLOCK);
    ESP_LOGI(
        TAG,
        "WAV prefetch=%us slot=%uB slots=%u PSRAM_total=%uB reader_priority=%u",
        (unsigned)CONFIG_AUDIO_MANAGER_WAV_PREFETCH_SECONDS,
        (unsigned)AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES,
        (unsigned)AUDIO_WAV_PREFETCH_SLOT_COUNT,
        (unsigned)(AUDIO_MANAGER_WAV_PREFETCH_SLOT_BYTES *
                   AUDIO_WAV_PREFETCH_SLOT_COUNT),
        (unsigned)AUDIO_MANAGER_WAV_PREFETCH_READER_PRIORITY);
    ESP_LOGI(
        TAG,
        "DSP HPF80x2 + LPF6kx2 + adaptive NS + 16x speaker conditioning + limiter");
    ESP_LOGI(TAG, "================================================");

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
    if (!already_running &&
        (s_runtime.task_handle == NULL) &&
        (s_runtime.wav_stress_task_handle == NULL))
    {
        s_control.task_running = true;
        s_control.shutdown_requested = false;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
        s_control.operation =
            (audio_manager_stability_mode_enabled() &&
             !audio_manager_mixed_stress_mode_enabled())
                ? AUDIO_MANAGER_OPERATION_STABILITY
                : AUDIO_MANAGER_OPERATION_NONE;
        s_runtime.wav_stress_shutdown_requested = false;
    }
    portEXIT_CRITICAL(&s_control_lock);

    if (already_running ||
        (s_runtime.task_handle != NULL) ||
        (s_runtime.wav_stress_task_handle != NULL))
    {
        xSemaphoreGive(s_runtime.status_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    (void)xQueueReset(s_runtime.command_queue);
    (void)xEventGroupClearBits(
        s_runtime.lifecycle_events,
        AUDIO_MANAGER_TASK_READY_BIT |
            AUDIO_MANAGER_TASK_STOPPED_BIT |
            AUDIO_MANAGER_WAV_STRESS_TASK_STOPPED_BIT |
            AUDIO_MANAGER_WAV_STRESS_TASK_SHUTDOWN_BIT);
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
        ESP_LOGE(TAG, "Manager task did not reach IDLE before timeout");
        (void)audio_manager_stop();
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Started and ready for commands");

#ifdef CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP
    const BaseType_t wav_stress_task_result = xTaskCreate(
        audio_manager_wav_stress_task,
        AUDIO_MANAGER_WAV_STRESS_TASK_NAME,
        AUDIO_MANAGER_WAV_STRESS_TASK_STACK_SIZE,
        NULL,
        AUDIO_MANAGER_WAV_STRESS_TASK_PRIORITY,
        &s_runtime.wav_stress_task_handle);
    if (wav_stress_task_result != pdPASS)
    {
        s_runtime.wav_stress_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create WAV stress coordinator task");
        (void)audio_manager_stop();
        return ESP_ERR_NO_MEM;
    }
#endif

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
        (s_runtime.status.state == AUDIO_MANAGER_STATE_IDLE))
    {
        s_control.operation = AUDIO_MANAGER_OPERATION_WAV;
        s_control.cancel_requested = false;
        s_control.record_stop_requested = false;
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
        audio_manager_finish_operation();
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
    portENTER_CRITICAL(&s_control_lock);
    if (s_control.task_running &&
        ((s_control.operation == AUDIO_MANAGER_OPERATION_WAV) ||
         (s_control.operation == AUDIO_MANAGER_OPERATION_RECORDED_PLAYBACK)))
    {
        s_control.cancel_requested = true;
        requested = true;
    }
    portEXIT_CRITICAL(&s_control_lock);

    xSemaphoreGive(s_runtime.status_mutex);
    return requested ? ESP_OK : ESP_ERR_INVALID_STATE;
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
    if ((current_task == s_runtime.task_handle) ||
        (current_task == s_runtime.wav_stress_task_handle))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!audio_manager_take_status_mutex("stopping manager task"))
    {
        return ESP_ERR_TIMEOUT;
    }

    bool task_running;
    bool first_request = false;
    bool wav_stress_task_running;
    portENTER_CRITICAL(&s_control_lock);
    task_running = s_control.task_running;
    wav_stress_task_running =
        (s_runtime.wav_stress_task_handle != NULL);
    if (task_running)
    {
        first_request = !s_control.shutdown_requested;
        s_control.shutdown_requested = true;
        s_control.cancel_requested = true;
        s_control.record_stop_requested = true;
    }
    s_runtime.wav_stress_shutdown_requested = true;
    portEXIT_CRITICAL(&s_control_lock);

    if (!task_running && !wav_stress_task_running)
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
            ESP_LOGW(
                TAG,
                "Shutdown command queue full; polling fallback active");
        }
    }

    if (wav_stress_task_running)
    {
        xEventGroupSetBits(
            s_runtime.lifecycle_events,
            AUDIO_MANAGER_WAV_STRESS_TASK_SHUTDOWN_BIT);
    }

    xSemaphoreGive(s_runtime.status_mutex);

    EventBits_t expected_stopped_bits = 0U;
    if (task_running)
    {
        expected_stopped_bits |= AUDIO_MANAGER_TASK_STOPPED_BIT;
    }
    if (wav_stress_task_running)
    {
        expected_stopped_bits |= AUDIO_MANAGER_WAV_STRESS_TASK_STOPPED_BIT;
    }

    const EventBits_t stopped_bits = xEventGroupWaitBits(
        s_runtime.lifecycle_events,
        expected_stopped_bits,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(AUDIO_MANAGER_TASK_STOP_TIMEOUT_MS));
    if ((stopped_bits & expected_stopped_bits) != expected_stopped_bits)
    {
        ESP_LOGE(
            TAG,
            "Audio task stop timed out; shutdown remains pending");
        return ESP_ERR_TIMEOUT;
    }

    if (wav_stress_task_running)
    {
        portENTER_CRITICAL(&s_control_lock);
        s_runtime.wav_stress_task_handle = NULL;
        portEXIT_CRITICAL(&s_control_lock);
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
        (s_runtime.wav_stress_task_handle != NULL) ||
        audio_wav_prefetch_is_active(
            &s_runtime.playback_source.wav_prefetch))
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = force_cycle_cleanup();

    heap_caps_free(s_runtime.recording_pcm24);
    heap_caps_free(s_runtime.dsp_workspace);
    s_runtime.recording_pcm24 = NULL;
    s_runtime.dsp_workspace = NULL;

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
