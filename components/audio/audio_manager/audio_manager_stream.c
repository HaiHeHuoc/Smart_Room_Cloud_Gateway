#include "audio_manager_stream.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define AUDIO_MANAGER_STREAM_LOCK_TIMEOUT_MS 100U

static SemaphoreHandle_t s_stream_lock = NULL;
static audio_manager_stream_frame_callback_t s_stream_callback = NULL;
static void *s_stream_callback_context = NULL;
static audio_manager_stream_status_t s_stream_status = {0};

static bool stream_take_lock(void)
{
    if (s_stream_lock == NULL)
    {
        s_stream_lock = xSemaphoreCreateMutex();
        if (s_stream_lock == NULL)
        {
            return false;
        }
    }

    return xSemaphoreTake(
               s_stream_lock,
               pdMS_TO_TICKS(AUDIO_MANAGER_STREAM_LOCK_TIMEOUT_MS)) == pdTRUE;
}

esp_err_t audio_manager_stream_register_callback(
    audio_manager_stream_frame_callback_t callback,
    void *user_context)
{
    if (!stream_take_lock())
    {
        return ESP_ERR_NO_MEM;
    }

    s_stream_callback = callback;
    s_stream_callback_context = user_context;
    xSemaphoreGive(s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_arm(uint32_t stream_generation)
{
    if (stream_generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!stream_take_lock())
    {
        return ESP_ERR_NO_MEM;
    }

    if (s_stream_status.armed)
    {
        xSemaphoreGive(s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_stream_status, 0, sizeof(s_stream_status));
    s_stream_status.armed = true;
    s_stream_status.stream_generation = stream_generation;
    xSemaphoreGive(s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_disarm(uint32_t stream_generation)
{
    if (stream_generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!stream_take_lock())
    {
        return ESP_ERR_NO_MEM;
    }

    if (!s_stream_status.armed ||
        (s_stream_status.stream_generation != stream_generation))
    {
        xSemaphoreGive(s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_stream_status.armed = false;
    xSemaphoreGive(s_stream_lock);
    return ESP_OK;
}

esp_err_t audio_manager_stream_get_status(
    audio_manager_stream_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!stream_take_lock())
    {
        return ESP_ERR_NO_MEM;
    }

    *status = s_stream_status;
    xSemaphoreGive(s_stream_lock);
    return ESP_OK;
}

/*
 * Internal producer hook for audio_manager.c.
 *
 * 14-B establishes this safe publication boundary; the exact producer call
 * site is connected in the next uplink checkpoint so transport integration and
 * PCM publication can be reviewed together. This function deliberately accepts
 * a copied PCM16 block and never exposes I2S/DMA/private PSRAM ownership.
 */
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

    if (!stream_take_lock())
    {
        return ESP_ERR_NO_MEM;
    }

    if (!s_stream_status.armed)
    {
        xSemaphoreGive(s_stream_lock);
        return ESP_ERR_INVALID_STATE;
    }

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
    if (callback == NULL)
    {
        ++s_stream_status.frames_dropped_no_callback;
    }

    xSemaphoreGive(s_stream_lock);

    if (callback != NULL)
    {
        callback(&frame, callback_context);
    }

    return ESP_OK;
}
