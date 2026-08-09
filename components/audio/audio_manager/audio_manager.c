/**
 * @file audio_manager.c
 * @brief Stability-first INMP441 -> DSP -> MAX98357A audio manager.
 *
 * The proven NewSolution audio path is intentionally preserved. This refactor
 * aligns lifecycle, status, callback, and state handling with the other
 * manager components in Smart Room Cloud Gateway so the same status model can
 * feed app_gui without coupling audio_manager to LVGL.
 */

/* Includes ----------------------------------------------------------------- */
#include "audio_manager.h"
#include "audio_dsp.h"
#include "audio_wav.h"
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
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Macros ------------------------------------------------------------------- */
#define AUDIO_MANAGER_DEFAULT_RECORD_SECONDS          5U
#define AUDIO_MANAGER_DEFAULT_VOLUME_PERCENT          100U

#define AUDIO_MANAGER_TASK_NAME                       "audio_manager"
#define AUDIO_MANAGER_TASK_STACK_SIZE                 8192U
#define AUDIO_MANAGER_TASK_PRIORITY                   5U
#define AUDIO_MANAGER_INTER_CYCLE_DELAY_MS            250U
#define AUDIO_MANAGER_MUTEX_TIMEOUT_MS                100U

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

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "AUDIO_MANAGER";

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    MICROPHONE_SLOT_LEFT = 0,
    MICROPHONE_SLOT_RIGHT = 1,
} microphone_slot_t;

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
    uint64_t data_bytes_read;
    uint64_t data_bytes_streamed;
    uint32_t expected_data_bytes;
    uint32_t expected_duration_ms;
    uint32_t read_count;
    uint32_t read_failure_count;
    uint32_t max_fread_duration_us;
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
    audio_wav_stream_t wav_stream;
} audio_playback_source_t;

typedef struct
{
    bool initialized;
    bool rx_enabled;
    bool tx_enabled;

    audio_manager_config_t config;

    /* One manager-owned source slot; only this task/lifecycle owns it. */
    audio_playback_source_t playback_source;

    size_t sample_capacity;
    size_t recording_bytes;
    int32_t *recording_pcm24;
    audio_dsp_workspace_t *dsp_workspace;

    i2s_chan_handle_t rx_channel;
    i2s_chan_handle_t tx_channel;

    TaskHandle_t task_handle;
    SemaphoreHandle_t status_mutex;

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
DMA_ATTR static const int16_t s_silence_block[
    AUDIO_MANAGER_FRAMES_PER_BLOCK * AUDIO_MANAGER_SLOT_COUNT] = {0};

/* Both ISR callbacks and tasks update these diagnostics. */
DRAM_ATTR static portMUX_TYPE s_diagnostics_lock =
    portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR static audio_manager_diagnostics_t s_diagnostics = {0};

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
static esp_err_t discard_microphone_startup(void);
static void update_slot_stats(slot_stats_t *stats, int32_t sample);
static esp_err_t detect_microphone_slot(microphone_slot_t *selected_slot);
static esp_err_t record_audio(
    microphone_slot_t selected_slot,
    size_t *samples_recorded);

static esp_err_t start_i2s_tx(void);
static esp_err_t stop_i2s_tx(void);
static esp_err_t write_tx_frames(size_t frame_count);
static esp_err_t write_silence_blocks(uint32_t block_count);
static int32_t apply_playback_volume_percent(int32_t sample_pcm24);
static int16_t decode_wav_pcm16_le(const uint8_t *sample_bytes);
static int16_t apply_wav_volume_percent(int16_t sample_pcm16);
static esp_err_t play_recording(
    size_t sample_count,
    audio_dsp_playback_stats_t *stats);
static esp_err_t play_wav_stream(
    audio_wav_stream_t *stream,
    audio_wav_playback_metrics_t *metrics);

static void dsp_cooperative_yield(void *context);
static void log_ns_metrics(const audio_dsp_ns_metrics_t *metrics);
static void log_playback_result(const audio_dsp_playback_stats_t *playback);

static esp_err_t record_once(size_t *samples_recorded);
static esp_err_t process_once(
    size_t sample_count,
    audio_cycle_metrics_t *metrics);
static esp_err_t playback_once(
    audio_playback_source_t *source,
    audio_cycle_metrics_t *metrics);
static esp_err_t audio_manager_select_recording_playback_source(
    size_t sample_count);
static esp_err_t audio_manager_select_wav_playback_source(const char *path);
static esp_err_t audio_manager_release_playback_source(void);
static esp_err_t force_cycle_cleanup(void);
static esp_err_t run_cycle(audio_cycle_metrics_t *metrics);
static const char *audio_manager_wav_validation_path(void);
static esp_err_t run_wav_validation_once(const char *path);
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

static esp_err_t discard_microphone_startup(void)
{
    for (uint32_t block = 0U;
         block < AUDIO_MANAGER_STARTUP_DISCARD_BLOCKS;
         ++block)
    {
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

static esp_err_t detect_microphone_slot(microphone_slot_t *selected_slot)
{
    if (selected_slot == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    slot_stats_t left = {0};
    slot_stats_t right = {0};

    for (uint32_t block = 0U;
         block < AUDIO_MANAGER_SLOT_DETECT_BLOCKS;
         ++block)
    {
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
    size_t *samples_recorded)
{
    if ((samples_recorded == NULL) ||
        (s_runtime.recording_pcm24 == NULL) ||
        (s_runtime.sample_capacity == 0U))
    {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t selected_slot_index =
        (selected_slot == MICROPHONE_SLOT_RIGHT) ? 1U : 0U;

    size_t captured = 0U;
    size_t next_progress = AUDIO_MANAGER_SAMPLE_RATE_HZ;

    while (captured < s_runtime.sample_capacity)
    {
        size_t frames_read = 0U;
        const esp_err_t result = read_rx_block(&frames_read);
        if (result != ESP_OK)
        {
            *samples_recorded = captured;
            return result;
        }

        const size_t remaining = s_runtime.sample_capacity - captured;
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
               (next_progress <= s_runtime.sample_capacity))
        {
            ESP_LOGI(
                TAG,
                "Recorded %u/%u seconds",
                (unsigned)(next_progress / AUDIO_MANAGER_SAMPLE_RATE_HZ),
                (unsigned)s_runtime.config.record_duration_seconds);
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

static esp_err_t write_silence_blocks(uint32_t block_count)
{
    memset(s_tx_block, 0, sizeof(s_tx_block));

    for (uint32_t block = 0U; block < block_count; ++block)
    {
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

    if (scaled > INT16_MAX)
    {
        return INT16_MAX;
    }
    if (scaled < INT16_MIN)
    {
        return INT16_MIN;
    }

    return (int16_t)scaled;
}

static esp_err_t play_recording(
    size_t sample_count,
    audio_dsp_playback_stats_t *stats)
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

static esp_err_t play_wav_stream(
    audio_wav_stream_t *stream,
    audio_wav_playback_metrics_t *metrics)
{
    if ((stream == NULL) ||
        (stream->file == NULL) ||
        (stream->buffer == NULL) ||
        (metrics == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(metrics, 0, sizeof(*metrics));
    metrics->expected_data_bytes = stream->info.data_size_bytes;
    metrics->expected_duration_ms = stream->info.duration_ms;

    esp_err_t result = ESP_OK;
    const int64_t playback_start_us = esp_timer_get_time();

    while (stream->data_bytes_remaining > 0U)
    {
        const uint8_t *pcm_bytes = NULL;
        size_t bytes_read = 0U;
        const int64_t read_start_us = esp_timer_get_time();
        const esp_err_t read_result = audio_wav_stream_read(
            stream,
            &pcm_bytes,
            &bytes_read);
        const int64_t read_duration_us =
            esp_timer_get_time() - read_start_us;
        const uint32_t bounded_read_duration_us =
            (read_duration_us <= 0)
                ? 0U
                : ((uint64_t)read_duration_us > UINT32_MAX)
                    ? UINT32_MAX
                    : (uint32_t)read_duration_us;

        ++metrics->read_count;
        if (bounded_read_duration_us > metrics->max_fread_duration_us)
        {
            metrics->max_fread_duration_us = bounded_read_duration_us;
        }

        if (read_result != ESP_OK)
        {
            ++metrics->read_failure_count;
            result = read_result;
            break;
        }

        if ((pcm_bytes == NULL) || (bytes_read == 0U))
        {
            ++metrics->read_failure_count;
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        if ((bytes_read % sizeof(int16_t)) != 0U)
        {
            ++metrics->read_failure_count;
            result = ESP_ERR_INVALID_SIZE;
            break;
        }

        metrics->data_bytes_read += bytes_read;
        const size_t sample_count = bytes_read / sizeof(int16_t);
        size_t sample_offset = 0U;

        while (sample_offset < sample_count)
        {
            size_t frames = sample_count - sample_offset;
            if (frames > AUDIO_MANAGER_FRAMES_PER_BLOCK)
            {
                frames = AUDIO_MANAGER_FRAMES_PER_BLOCK;
            }

            for (size_t frame = 0U; frame < frames; ++frame)
            {
                const size_t sample_byte_offset =
                    (sample_offset + frame) * sizeof(int16_t);
                const int16_t mono_sample = apply_wav_volume_percent(
                    decode_wav_pcm16_le(&pcm_bytes[sample_byte_offset]));
                const size_t slot = frame * AUDIO_MANAGER_SLOT_COUNT;
                s_tx_block[slot] = mono_sample;
                s_tx_block[slot + 1U] = mono_sample;
            }

            result = write_tx_frames(frames);
            if (result != ESP_OK)
            {
                break;
            }

            metrics->data_bytes_streamed += frames * sizeof(int16_t);
            sample_offset += frames;
        }

        if (result != ESP_OK)
        {
            break;
        }
    }

    const int64_t elapsed_us = esp_timer_get_time() - playback_start_us;
    metrics->playback_elapsed_ms =
        (elapsed_us <= 0)
            ? 0U
            : ((uint64_t)elapsed_us / 1000U > UINT32_MAX)
                ? UINT32_MAX
                : (uint32_t)((uint64_t)elapsed_us / 1000U);

    if ((result == ESP_OK) &&
        ((metrics->data_bytes_read != metrics->expected_data_bytes) ||
         (metrics->data_bytes_streamed != metrics->expected_data_bytes)))
    {
        result = ESP_ERR_INVALID_SIZE;
    }

    return result;
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

static esp_err_t record_once(size_t *samples_recorded)
{
    if (samples_recorded == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *samples_recorded = 0U;
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

    result = discard_microphone_startup();
    if (result == ESP_OK)
    {
        result = detect_microphone_slot(&selected_slot);
    }

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "RECORDING %u seconds",
            (unsigned)s_runtime.config.record_duration_seconds);
        result = record_audio(selected_slot, samples_recorded);
    }

    const esp_err_t stop_result = stop_i2s_rx();
    if ((result == ESP_OK) && (stop_result != ESP_OK))
    {
        result = stop_result;
    }

    audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    return result;
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
    audio_cycle_metrics_t *metrics)
{
    if ((source == NULL) || (metrics == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

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
            if ((source->wav_stream.file == NULL) ||
                (source->wav_stream.buffer == NULL))
            {
                return ESP_ERR_INVALID_STATE;
            }
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = start_i2s_tx();
    if (result != ESP_OK)
    {
        return result;
    }

    result = write_silence_blocks(
        AUDIO_MANAGER_PRE_PLAYBACK_SILENCE_BLOCKS);

    if (result == ESP_OK)
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_PLAYBACK);

        if (source->kind == AUDIO_PLAYBACK_SOURCE_RECORDED_PCM24)
        {
            ESP_LOGI(
                TAG,
                "PLAYBACK samples=%u volume=%u/100 gain=%u.%02ux limiter=+/-%d",
                (unsigned)source->recorded_sample_count,
                (unsigned)s_runtime.config.playback_volume_percent,
                (unsigned)(AUDIO_DSP_PLAYBACK_GAIN_Q8 / AUDIO_DSP_Q8_ONE),
                (unsigned)(((AUDIO_DSP_PLAYBACK_GAIN_Q8 %
                              AUDIO_DSP_Q8_ONE) * 100U) /
                            AUDIO_DSP_Q8_ONE),
                AUDIO_DSP_PLAYBACK_PEAK_LIMIT_PCM16);
            result = play_recording(
                source->recorded_sample_count,
                &metrics->playback);
        }
        else
        {
            ESP_LOGI(
                TAG,
                "WAV PLAYBACK data_bytes=%u duration=%ums volume=%u/100 policy=linear_pcm16",
                (unsigned)source->wav_stream.info.data_size_bytes,
                (unsigned)source->wav_stream.info.duration_ms,
                (unsigned)s_runtime.config.playback_volume_percent);
            result = play_wav_stream(
                &source->wav_stream,
                &metrics->wav);
        }
    }

    if (result == ESP_OK)
    {
        result = write_silence_blocks(
            AUDIO_MANAGER_POST_PLAYBACK_SILENCE_BLOCKS);
    }

    const esp_err_t stop_result = stop_i2s_tx();
    if ((result == ESP_OK) && (stop_result != ESP_OK))
    {
        result = stop_result;
    }

    if ((result == ESP_OK) &&
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

    if ((s_runtime.playback_source.wav_stream.file != NULL) ||
        (s_runtime.playback_source.wav_stream.buffer != NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t result = audio_wav_stream_open(
        &s_runtime.playback_source.wav_stream,
        path);
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
        (s_runtime.playback_source.wav_stream.file != NULL) ||
        (s_runtime.playback_source.wav_stream.buffer != NULL))
    {
        result = audio_wav_stream_close(&s_runtime.playback_source.wav_stream);
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

    audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
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
        (samples_recorded != s_runtime.sample_capacity))
    {
        ESP_LOGE(
            TAG,
            "Recording incomplete: got=%u expected=%u",
            (unsigned)samples_recorded,
            (unsigned)s_runtime.sample_capacity);
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
        vTaskDelay(pdMS_TO_TICKS(AUDIO_MANAGER_PRE_PLAYBACK_DELAY_MS));
        result = playback_once(&s_runtime.playback_source, metrics);
    }

    metrics->samples_recorded = samples_recorded;

    const esp_err_t cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (cleanup_result != ESP_OK))
    {
        result = cleanup_result;
    }

    return result;
}

static const char *audio_manager_wav_validation_path(void)
{
#ifdef CONFIG_AUDIO_MANAGER_WAV_VALIDATION_ONCE
    return CONFIG_AUDIO_MANAGER_WAV_VALIDATION_PATH;
#else
    return NULL;
#endif
}

static esp_err_t run_wav_validation_once(const char *path)
{
    if ((path == NULL) || (path[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "========== WAV VALIDATION ONCE ==========");
    ESP_LOGI(TAG, "WAV validation path=%s", path);

    audio_manager_diagnostics_t diagnostics_before = {0};
    audio_manager_snapshot_diagnostics(&diagnostics_before);

    audio_cycle_metrics_t metrics = {0};
    uint32_t expected_data_bytes = 0U;
    uint32_t expected_duration_ms = 0U;

    esp_err_t result = audio_manager_select_wav_playback_source(path);
    if (result == ESP_OK)
    {
        expected_data_bytes =
            s_runtime.playback_source.wav_stream.info.data_size_bytes;
        expected_duration_ms =
            s_runtime.playback_source.wav_stream.info.duration_ms;
        result = playback_once(&s_runtime.playback_source, &metrics);
    }

    const esp_err_t cleanup_result = force_cycle_cleanup();
    if ((result == ESP_OK) && (cleanup_result != ESP_OK))
    {
        result = cleanup_result;
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
        "WAV_DIAG result=%s expected_bytes=%u duration=%ums read_bytes=%llu streamed_bytes=%llu reads=%u read_fail=%u max_fread_us=%u elapsed=%ums tx_requested=%llu tx_written=%llu tx_q_ovf=%u tx_timeout=%u tx_partial=%u max_tx_us=%u",
        esp_err_to_name(result),
        (unsigned)expected_data_bytes,
        (unsigned)expected_duration_ms,
        (unsigned long long)metrics.wav.data_bytes_read,
        (unsigned long long)metrics.wav.data_bytes_streamed,
        (unsigned)metrics.wav.read_count,
        (unsigned)metrics.wav.read_failure_count,
        (unsigned)metrics.wav.max_fread_duration_us,
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

    if (audio_manager_take_status_mutex("storing WAV validation result"))
    {
        s_runtime.status.last_error = result;
        audio_manager_refresh_diagnostics_locked();
        xSemaphoreGive(s_runtime.status_mutex);
    }

    if (result == ESP_OK)
    {
        audio_manager_notify_status_changed();
        ESP_LOGI(TAG, "WAV validation PASS");
    }
    else
    {
        audio_manager_set_state(AUDIO_MANAGER_STATE_ERROR);
        ESP_LOGE(
            TAG,
            "WAV validation FAIL: %s",
            esp_err_to_name(result));
        audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
    }

    return result;
}

/* Static Functions: Stability Task ---------------------------------------- */
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

static void audio_manager_task(void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Audio manager task started: record=%us volume=%u/100",
        (unsigned)s_runtime.config.record_duration_seconds,
        (unsigned)s_runtime.config.playback_volume_percent);

    const char *const wav_validation_path =
        audio_manager_wav_validation_path();
    if (wav_validation_path != NULL)
    {
        (void)run_wav_validation_once(wav_validation_path);
        log_heap_state("wav_validation_end");
    }

    while (true)
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

        log_cycle_diagnostics(
            cycle,
            &diagnostics_before);
        log_heap_state("cycle_end");

        if (result != ESP_OK)
        {
            /* Preserve last_error/counters but recover lifecycle for next cycle. */
            audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_MANAGER_INTER_CYCLE_DELAY_MS));
    }
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

    const size_t sample_capacity =
        (size_t)AUDIO_MANAGER_SAMPLE_RATE_HZ *
        (size_t)config->record_duration_seconds;

    if ((sample_capacity / AUDIO_MANAGER_SAMPLE_RATE_HZ) !=
        config->record_duration_seconds)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (sample_capacity > (SIZE_MAX / sizeof(int32_t)))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.config = *config;
    s_runtime.sample_capacity = sample_capacity;
    s_runtime.recording_bytes = sample_capacity * sizeof(int32_t);

    s_runtime.status_mutex = xSemaphoreCreateMutex();
    if (s_runtime.status_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = hold_amplifier_data_low();
    if (result != ESP_OK)
    {
        vSemaphoreDelete(s_runtime.status_mutex);
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
        vSemaphoreDelete(s_runtime.status_mutex);
        s_runtime.status_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(s_runtime.recording_pcm24, 0, s_runtime.recording_bytes);
    audio_dsp_workspace_init(s_runtime.dsp_workspace);

    audio_manager_reset_diagnostics();

    s_runtime.initialized = true;
    s_runtime.status = (audio_manager_status_t) {
        .state = AUDIO_MANAGER_STATE_INITIALIZED,
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
        "Config sample_rate=%u record=%us samples=%u PCM24_PSRAM=%uB volume=%u/100 DMA=%ux%u",
        (unsigned)AUDIO_MANAGER_SAMPLE_RATE_HZ,
        (unsigned)config->record_duration_seconds,
        (unsigned)s_runtime.sample_capacity,
        (unsigned)s_runtime.recording_bytes,
        (unsigned)config->playback_volume_percent,
        (unsigned)AUDIO_MANAGER_DMA_DESC_NUM,
        (unsigned)AUDIO_MANAGER_FRAMES_PER_BLOCK);
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
    if (!s_runtime.initialized || (s_runtime.status_mutex == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_runtime.task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_manager_set_state(AUDIO_MANAGER_STATE_IDLE);

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
        audio_manager_set_state(AUDIO_MANAGER_STATE_INITIALIZED);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Started");
    return ESP_OK;
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

    if (s_runtime.task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = force_cycle_cleanup();

    heap_caps_free(s_runtime.recording_pcm24);
    heap_caps_free(s_runtime.dsp_workspace);
    s_runtime.recording_pcm24 = NULL;
    s_runtime.dsp_workspace = NULL;

    if (s_runtime.status_mutex != NULL)
    {
        vSemaphoreDelete(s_runtime.status_mutex);
    }

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

esp_err_t audio_manager_test_start(void)
{
    return audio_manager_start();
}
