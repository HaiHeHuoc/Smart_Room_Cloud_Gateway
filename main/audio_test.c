/* Includes ----------------------------------------------------------------- */
/*
 * One-shot microphone diagnostic. The captured PCM buffer remains private to
 * this module; it is allocated in PSRAM so normal application services retain
 * Internal RAM headroom.
 */

#include "audio_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "board_config.h"

#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_memory_utils.h"

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/* Macros ------------------------------------------------------------------- */

#define AUDIO_TEST_SAMPLE_RATE_HZ       16000U
#define AUDIO_TEST_RECORD_SECONDS       5U

#define AUDIO_TEST_SLOT_COUNT           2U
#define AUDIO_TEST_FRAMES_PER_BLOCK     256U
#define AUDIO_TEST_I2S_TIMEOUT_MS       1000U

#define AUDIO_TEST_STARTUP_DISCARD_BLOCKS   40U
#define AUDIO_TEST_SLOT_DETECT_BLOCKS       20U

#define AUDIO_TEST_BUFFER_SAMPLES       \
    ((size_t)AUDIO_TEST_SAMPLE_RATE_HZ * \
     (size_t)AUDIO_TEST_RECORD_SECONDS)

#define AUDIO_TEST_BUFFER_SIZE_BYTES    \
    (AUDIO_TEST_BUFFER_SAMPLES * sizeof(int32_t))

_Static_assert(
    AUDIO_TEST_SLOT_COUNT == 2U,
    "Standard I2S transport requires two slots");

/* Constants ---------------------------------------------------------------- */

static const char *const TAG = "AUDIO_TEST";

/* Type Definitions --------------------------------------------------------- */

typedef enum
{
    AUDIO_TEST_MIC_SLOT_LEFT = 0,
    AUDIO_TEST_MIC_SLOT_RIGHT,
} audio_test_mic_slot_t;

typedef struct
{
    uint64_t absolute_sum;
    uint32_t peak;
    uint32_t sample_count;
} audio_test_slot_stats_t;

/* Static Variables --------------------------------------------------------- */

/* Lifetime-owned capture resources; access is serialized by the test task. */
static int32_t *s_audio_buffer = NULL;
static bool s_initialized = false;
static i2s_chan_handle_t s_rx_channel = NULL;

/* ISR-updated overflow counter; read after the I2S channel has stopped. */
static volatile uint32_t s_rx_overflow_count = 0U;

/* One interleaved stereo DMA block, kept in Internal RAM for I2S access. */
static int32_t s_rx_block[
    AUDIO_TEST_FRAMES_PER_BLOCK *
    AUDIO_TEST_SLOT_COUNT];

/* Capture timing diagnostics; they are reset at the start of each capture. */
static int64_t s_last_rx_read_us = 0;
static int64_t s_max_rx_read_gap_us = 0;
static int64_t s_max_rx_read_duration_us = 0;
static int64_t s_max_rx_processing_gap_us = 0;
static int64_t s_last_rx_read_return_us = 0;

/* Function Prototypes ------------------------------------------------------ */

/** @brief Configure the amplifier data pin so the speaker remains silent. */
static esp_err_t audio_test_configure_amplifier_safe_state(void);

/** @brief Count I2S receive-queue overflows without performing blocking work. */
static bool audio_test_rx_overflow_callback(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_ctx);

/** @brief Log allocatable Internal RAM, DMA-capable RAM, and PSRAM. */
static void audio_test_log_memory(
    const char *stage);

/** @brief Create, configure, and enable the temporary I2S receive channel. */
static esp_err_t audio_test_start_i2s_rx(void);

/** @brief Disable and destroy the temporary I2S receive channel, if present. */
static esp_err_t audio_test_stop_i2s_rx(void);

/** @brief Read one frame-aligned stereo block from I2S. */
static esp_err_t audio_test_read_rx_block(
    size_t *frames_read);

/** @brief Discard unstable microphone samples immediately after startup. */
static esp_err_t audio_test_discard_microphone_startup(void);

/** @brief Convert an I2S slot value to a sign-extended 24-bit PCM sample. */
static int32_t audio_test_raw_slot_to_pcm24(
    int32_t raw_slot);

/** @brief Accumulate peak and mean-magnitude statistics for one slot. */
static void audio_test_update_slot_stats(
    audio_test_slot_stats_t *stats,
    int32_t sample);

/** @brief Select the active microphone slot from short stereo observations. */
static esp_err_t audio_test_detect_microphone_slot(
    audio_test_mic_slot_t *selected_slot);

/** @brief Capture the configured duration from the selected microphone slot. */
static esp_err_t audio_test_capture(
    audio_test_mic_slot_t selected_slot,
    size_t *samples_recorded);

/* Static Functions --------------------------------------------------------- */

static bool IRAM_ATTR audio_test_rx_overflow_callback(
    i2s_chan_handle_t handle,
    i2s_event_data_t *event,
    void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;

    s_rx_overflow_count++;

    return false;
}

static esp_err_t audio_test_capture(
    audio_test_mic_slot_t selected_slot,
    size_t *samples_recorded)
{
    ESP_RETURN_ON_FALSE(
        s_audio_buffer != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Audio buffer is NULL");

    ESP_RETURN_ON_FALSE(
        samples_recorded != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "samples_recorded is NULL");

    const size_t slot_index =
        (selected_slot ==
         AUDIO_TEST_MIC_SLOT_RIGHT)
            ? 1U
            : 0U;

    size_t captured = 0U;

    size_t next_progress =
        AUDIO_TEST_SAMPLE_RATE_HZ;

    while (captured <
           AUDIO_TEST_BUFFER_SAMPLES)
    {
        size_t frames_read = 0U;

        const esp_err_t ret =
            audio_test_read_rx_block(
                &frames_read);

        if (ret != ESP_OK)
        {
            *samples_recorded =
                captured;

            return ret;
        }

        const size_t remaining =
            AUDIO_TEST_BUFFER_SAMPLES -
            captured;

        if (frames_read > remaining)
        {
            frames_read = remaining;
        }

        for (size_t frame = 0U;
             frame < frames_read;
             ++frame)
        {
            const size_t source =
                frame *
                AUDIO_TEST_SLOT_COUNT +
                slot_index;

            s_audio_buffer[
                captured + frame] =
                audio_test_raw_slot_to_pcm24(
                    s_rx_block[source]);
        }

        captured += frames_read;

        while ((captured >= next_progress) &&
               (next_progress <=
                AUDIO_TEST_BUFFER_SAMPLES))
        {
            ESP_LOGI(
                TAG,
                "Recorded %lu/%u seconds",
                (unsigned long)(
                    next_progress /
                    AUDIO_TEST_SAMPLE_RATE_HZ),
                AUDIO_TEST_RECORD_SECONDS);

            next_progress +=
                AUDIO_TEST_SAMPLE_RATE_HZ;
        }
    }

    *samples_recorded = captured;

    return ESP_OK;
}

static void audio_test_update_slot_stats(
    audio_test_slot_stats_t *stats,
    int32_t sample)
{
    if (stats == NULL)
    {
        return;
    }

    const uint32_t magnitude =
        (sample < 0)
            ? (uint32_t)(-(int64_t)sample)
            : (uint32_t)sample;

    stats->absolute_sum += magnitude;
    stats->sample_count++;

    if (magnitude > stats->peak)
    {
        stats->peak = magnitude;
    }
}

static esp_err_t audio_test_detect_microphone_slot(
    audio_test_mic_slot_t *selected_slot)
{
    ESP_RETURN_ON_FALSE(
        selected_slot != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "selected_slot is NULL");

    audio_test_slot_stats_t left = {0};
    audio_test_slot_stats_t right = {0};

    for (uint32_t block = 0U;
         block < AUDIO_TEST_SLOT_DETECT_BLOCKS;
         ++block)
    {
        size_t frames_read = 0U;

        ESP_RETURN_ON_ERROR(
            audio_test_read_rx_block(
                &frames_read),
            TAG,
            "Failed to read RX block during slot detection");

        for (size_t frame = 0U;
             frame < frames_read;
             ++frame)
        {
            const size_t base =
                frame *
                AUDIO_TEST_SLOT_COUNT;

            const int32_t left_sample =
                audio_test_raw_slot_to_pcm24(
                    s_rx_block[base]);

            const int32_t right_sample =
                audio_test_raw_slot_to_pcm24(
                    s_rx_block[base + 1U]);

            audio_test_update_slot_stats(
                &left,
                left_sample);

            audio_test_update_slot_stats(
                &right,
                right_sample);
        }
    }

    const uint64_t left_average =
        (left.sample_count == 0U)
            ? 0U
            : left.absolute_sum /
              left.sample_count;

    const uint64_t right_average =
        (right.sample_count == 0U)
            ? 0U
            : right.absolute_sum /
              right.sample_count;

    *selected_slot =
        (right_average > left_average)
            ? AUDIO_TEST_MIC_SLOT_RIGHT
            : AUDIO_TEST_MIC_SLOT_LEFT;

    ESP_LOGI(
        TAG,
        "SLOT_DETECT: "
        "left_avg=%llu left_peak=%lu "
        "right_avg=%llu right_peak=%lu "
        "selected=%s",
        (unsigned long long)left_average,
        (unsigned long)left.peak,
        (unsigned long long)right_average,
        (unsigned long)right.peak,
        (*selected_slot ==
         AUDIO_TEST_MIC_SLOT_LEFT)
            ? "LEFT"
            : "RIGHT");

    return ESP_OK;
}



static esp_err_t audio_test_discard_microphone_startup(void)
{
    for (uint32_t block = 0U;
         block < AUDIO_TEST_STARTUP_DISCARD_BLOCKS;
         ++block)
    {
        size_t frames_read = 0U;

        const esp_err_t ret =
            audio_test_read_rx_block(
                &frames_read);

        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    return ESP_OK;
}

static int32_t audio_test_raw_slot_to_pcm24(
    int32_t raw_slot)
{
    uint32_t sample =
        ((uint32_t)raw_slot >> 8U) &
        0x00FFFFFFU;

    if ((sample & 0x00800000U) != 0U)
    {
        sample |= 0xFF000000U;
    }

    return (int32_t)sample;
}



static esp_err_t audio_test_read_rx_block(
    size_t *frames_read)
{
    ESP_RETURN_ON_FALSE(
        frames_read != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "frames_read is NULL");

    ESP_RETURN_ON_FALSE(
        s_rx_channel != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "RX channel is not started");

    size_t bytes_read = 0U;
    
    const int64_t now_us = esp_timer_get_time();

    if (s_last_rx_read_us != 0)
    {
        const int64_t gap_us =
            now_us - s_last_rx_read_us;

        if (gap_us > s_max_rx_read_gap_us)
        {
            s_max_rx_read_gap_us = gap_us;
        }
    }

    s_last_rx_read_us = now_us;

    const int64_t read_start_us =
        esp_timer_get_time();

    const esp_err_t ret =
        i2s_channel_read(
            s_rx_channel,
            s_rx_block,
            sizeof(s_rx_block),
            &bytes_read,
            pdMS_TO_TICKS(
                AUDIO_TEST_I2S_TIMEOUT_MS));

    if (s_last_rx_read_return_us != 0)
    {
        const int64_t processing_gap_us =
            read_start_us -
            s_last_rx_read_return_us;

        if (processing_gap_us >
            s_max_rx_processing_gap_us)
        {
            s_max_rx_processing_gap_us =
                processing_gap_us;
        }
    }

    if (ret != ESP_OK)
    {
        *frames_read = 0U;
        return ret;
    }

    const int64_t read_end_us =
        esp_timer_get_time();

    const int64_t read_duration_us =
        read_end_us - read_start_us;

    if (read_duration_us >
        s_max_rx_read_duration_us)
    {
        s_max_rx_read_duration_us =
            read_duration_us;
    }

    s_last_rx_read_return_us = read_end_us;

    const size_t bytes_per_frame =
        AUDIO_TEST_SLOT_COUNT *
        sizeof(int32_t);

    if ((bytes_read % bytes_per_frame) != 0U)
    {
        ESP_LOGW(
            TAG,
            "RX bytes are not frame aligned: %lu",
            (unsigned long)bytes_read);
    }

    *frames_read =
        bytes_read / bytes_per_frame;

    return ESP_OK;
}

static esp_err_t audio_test_stop_i2s_rx(void)
{
    if (s_rx_channel == NULL)
    {
        return ESP_OK;
    }

    const esp_err_t disable_ret =
        i2s_channel_disable(
            s_rx_channel);

    const esp_err_t delete_ret =
        i2s_del_channel(
            s_rx_channel);

    s_rx_channel = NULL;

    if (disable_ret != ESP_OK)
    {
        return disable_ret;
    }

    return delete_ret;
}


static esp_err_t audio_test_start_i2s_rx(void)
{
    if (s_rx_channel != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_0,
            I2S_ROLE_MASTER);

    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num =
        AUDIO_TEST_FRAMES_PER_BLOCK;

    esp_err_t ret =
        i2s_new_channel(
            &channel_config,
            NULL,
            &s_rx_channel);

    if (ret != ESP_OK)
    {
        s_rx_channel = NULL;
        return ret;
    }

    i2s_event_callbacks_t callbacks = {
    .on_recv = NULL,
    .on_recv_q_ovf = audio_test_rx_overflow_callback,
    .on_sent = NULL,
    .on_send_q_ovf = NULL,
    };

    ret = i2s_channel_register_event_callback(
        s_rx_channel,
        &callbacks,
        (void*)&s_rx_overflow_count
    );

    if (ret != ESP_OK)
    {
        (void)i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        return ret;
    }

    i2s_std_config_t i2s_config =
    {
        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(
                AUDIO_TEST_SAMPLE_RATE_HZ),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_32BIT,
                I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk =
                    I2S_GPIO_UNUSED,

                .bclk =
                    AUDIO_GPIO_BCLK,

                .ws =
                    AUDIO_GPIO_WS,

                .dout =
                    I2S_GPIO_UNUSED,

                .din =
                    AUDIO_GPIO_MIC_DIN,

                .invert_flags =
                {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
    };

    i2s_config.slot_cfg.slot_mask =
        I2S_STD_SLOT_BOTH;

    ret =
        i2s_channel_init_std_mode(
            s_rx_channel,
            &i2s_config);

    if (ret == ESP_OK)
    {
        ret =
            i2s_channel_enable(
                s_rx_channel);
    }

    if (ret != ESP_OK)
    {
        (void)i2s_del_channel(
            s_rx_channel);

        s_rx_channel = NULL;
    }

    return ret;
}

static esp_err_t audio_test_configure_amplifier_safe_state(void)
{
    const gpio_config_t m_gpio_config = {
        .pin_bit_mask =
            (1ULL << AUDIO_GPIO_SPK_DOUT),

        .mode =
            GPIO_MODE_OUTPUT,

        .pull_up_en =
            GPIO_PULLUP_DISABLE,

        .pull_down_en =
            GPIO_PULLDOWN_ENABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&m_gpio_config),
        TAG,
        "Failed to configure amplifier DIN GPIO");

    ESP_RETURN_ON_ERROR(
        gpio_set_level(
            AUDIO_GPIO_SPK_DOUT,
            0),
        TAG,
        "Failed to hold amplifier DIN LOW");

    return ESP_OK;
}


static void audio_test_log_memory(
    const char *stage)
{
    const size_t internal_free =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT);

    const size_t internal_largest =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT);

    const size_t dma_free =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_DMA);

    const size_t dma_largest =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_DMA);

    const size_t psram_free =
        heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT);

    const size_t psram_largest =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT);

    ESP_LOGI(
        TAG,
        "[%s] INTERNAL free=%lu largest=%lu",
        stage,
        (unsigned long)internal_free,
        (unsigned long)internal_largest);

    ESP_LOGI(
        TAG,
        "[%s] DMA free=%lu largest=%lu",
        stage,
        (unsigned long)dma_free,
        (unsigned long)dma_largest);

    ESP_LOGI(
        TAG,
        "[%s] PSRAM free=%lu largest=%lu",
        stage,
        (unsigned long)psram_free,
        (unsigned long)psram_largest);
}


/* Functions ---------------------------------------------------------------- */

/**
 * @brief Allocate diagnostic resources and place the amplifier in safe state.
 *
 * This function is idempotent. It must run in task context before capture.
 */
esp_err_t audio_test_init(void)
{
    if (s_initialized)
    {
        ESP_LOGW(
            TAG,
            "Audio test is already initialized");

        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Initializing audio coexistence test");

    ESP_RETURN_ON_ERROR(
        audio_test_configure_amplifier_safe_state(),
        TAG,
        "Failed to configure amplifier safe state");

    audio_test_log_memory("before allocation");

    s_audio_buffer =
            heap_caps_malloc(
                AUDIO_TEST_BUFFER_SIZE_BYTES,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT);

    ESP_RETURN_ON_FALSE(
        s_audio_buffer != NULL,
        ESP_ERR_NO_MEM,
        TAG,
        "Failed to allocate %lu bytes in PSRAM",
        (unsigned long)AUDIO_TEST_BUFFER_SIZE_BYTES);

    if (!esp_ptr_external_ram(s_audio_buffer))
    {
        ESP_LOGE(
            TAG,
            "Audio buffer was not allocated in PSRAM");

        free(s_audio_buffer);
        s_audio_buffer = NULL;

        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Audio buffer allocated in PSRAM: "
        "samples=%lu bytes=%lu address=%p",
        (unsigned long)AUDIO_TEST_BUFFER_SAMPLES,
        (unsigned long)AUDIO_TEST_BUFFER_SIZE_BYTES,
        (void *)s_audio_buffer);

    audio_test_log_memory("after allocation");

    s_initialized = true;

    ESP_LOGI(
        TAG,
        "Audio coexistence test initialized");

    return ESP_OK;
}

/**
 * @brief Release diagnostic resources and leave the amplifier data pin low.
 *
 * Safe to call after a partial initialization or failed capture.
 */
esp_err_t audio_test_deinit(void)
{
    const esp_err_t rx_ret =
        audio_test_stop_i2s_rx();

    if (rx_ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to stop RX during deinit: %s",
            esp_err_to_name(rx_ret));
    }

    if (!s_initialized)
    {
        return ESP_OK;
    }

    ESP_LOGI(
        TAG,
        "Deinitializing audio coexistence test");

    ESP_RETURN_ON_ERROR(
        gpio_set_level(
            AUDIO_GPIO_SPK_DOUT,
            0),
        TAG,
        "Failed to hold amplifier DIN LOW");

    if (s_audio_buffer != NULL)
    {
        free(s_audio_buffer);
        s_audio_buffer = NULL;
    }

    s_initialized = false;

    audio_test_log_memory("after deinit");

    ESP_LOGI(
        TAG,
        "Audio coexistence test deinitialized");

    return ESP_OK;
}

/**
 * @brief Perform a synchronous one-shot microphone capture.
 *
 * The call owns the I2S receive channel until capture cleanup completes.
 */
esp_err_t audio_test_record_once(
    size_t *samples_recorded)
{
    ESP_LOGI(
        TAG,
        "Capture task: name=%s priority=%u core=%d",
        pcTaskGetName(NULL),
        (unsigned)uxTaskPriorityGet(NULL),
        xPortGetCoreID());

    ESP_RETURN_ON_FALSE(
        s_initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Audio test is not initialized");

    ESP_RETURN_ON_FALSE(
        s_audio_buffer != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Audio buffer is not allocated");

    size_t captured = 0U;

    ESP_LOGI(
        TAG,
        "Starting microphone capture");

    ESP_RETURN_ON_ERROR(
        gpio_set_level(
            AUDIO_GPIO_SPK_DOUT,
            0),
        TAG,
        "Failed to hold amplifier DIN LOW");

    s_rx_overflow_count = 0U;
    s_last_rx_read_us = 0;
    s_max_rx_read_gap_us = 0;

    s_last_rx_read_return_us = 0;
    s_max_rx_read_duration_us = 0;
    s_max_rx_processing_gap_us = 0;

    esp_err_t ret =
        audio_test_start_i2s_rx();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start I2S RX: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ret =
        audio_test_discard_microphone_startup();

    audio_test_mic_slot_t slot =
        AUDIO_TEST_MIC_SLOT_LEFT;

    if (ret == ESP_OK)
    {
        ret =
            audio_test_detect_microphone_slot(
                &slot);
    }

    if (ret == ESP_OK)
    {
        ret =
            audio_test_capture(
                slot,
                &captured);
    }

    const esp_err_t stop_ret =
        audio_test_stop_i2s_rx();

    const uint32_t overflow_count =
        s_rx_overflow_count;

    const int64_t max_read_gap_us =
        s_max_rx_read_gap_us;

    if ((ret == ESP_OK) &&
        (stop_ret != ESP_OK))
    {
        ret = stop_ret;
    }

    if (samples_recorded != NULL)
    {
        *samples_recorded =
            captured;
    }

    if (ret == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Microphone capture completed: "
            "samples=%lu expected=%lu",
            (unsigned long)captured,
            (unsigned long)
                AUDIO_TEST_BUFFER_SAMPLES);
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Microphone capture failed: %s",
            esp_err_to_name(ret));
    }

    audio_test_log_memory(
        "after RX capture");

    ESP_LOGI(
    TAG,
    "RX overflow count: %lu",
    (unsigned long)overflow_count);

    ESP_LOGI(
        TAG,
        "RX max read gap: %lld us (%.2f ms)",
        max_read_gap_us,
        (double)max_read_gap_us / 1000.0);

ESP_LOGI(
    TAG,
    "RX max read duration: %lld us (%.2f ms)",
    s_max_rx_read_duration_us,
    (double)s_max_rx_read_duration_us / 1000.0);

ESP_LOGI(
    TAG,
    "RX max processing gap: %lld us (%.2f ms)",
    s_max_rx_processing_gap_us,
    (double)s_max_rx_processing_gap_us / 1000.0);

    return ret;
}
