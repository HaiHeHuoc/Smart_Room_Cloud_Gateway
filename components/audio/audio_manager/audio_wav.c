/**
 * @file audio_wav.c
 * @brief Private bounded RIFF/WAVE reader for the mounted SD filesystem.
 */

/* Includes ----------------------------------------------------------------- */
#include "audio_wav.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "sd_card_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#endif

/* Macros ------------------------------------------------------------------- */
#define AUDIO_WAV_RIFF_HEADER_BYTES       12U
#define AUDIO_WAV_CHUNK_HEADER_BYTES       8U
#define AUDIO_WAV_FMT_MIN_BYTES            16U
#define AUDIO_WAV_PCM_FORMAT                1U
#define AUDIO_WAV_MONO_CHANNELS             1U
#define AUDIO_WAV_SAMPLE_RATE_HZ        16000U
#define AUDIO_WAV_BITS_PER_SAMPLE          16U
#define AUDIO_WAV_BLOCK_ALIGN_BYTES         2U
#define AUDIO_WAV_BYTE_RATE             AUDIO_WAV_CANONICAL_BYTE_RATE

#ifdef ESP_PLATFORM
#define AUDIO_WAV_PREFETCH_BUFFER_COUNT        2U
#define AUDIO_WAV_PREFETCH_TASK_NAME          "wav_prefetch"
#define AUDIO_WAV_PREFETCH_TASK_STACK_SIZE    4096U
#define AUDIO_WAV_PREFETCH_TASK_PRIORITY         4U
#define AUDIO_WAV_PREFETCH_START_TIMEOUT_MS   5000U
#define AUDIO_WAV_PREFETCH_WAIT_TIMEOUT_MS    5000U
#define AUDIO_WAV_PREFETCH_STOP_TIMEOUT_MS    5000U
#define AUDIO_WAV_PREFETCH_STALL_WARN_US      2000U

#define AUDIO_WAV_EVENT_BUFFER0_FREE  ((EventBits_t)(1U << 0U))
#define AUDIO_WAV_EVENT_BUFFER1_FREE  ((EventBits_t)(1U << 1U))
#define AUDIO_WAV_EVENT_BUFFER0_READY ((EventBits_t)(1U << 2U))
#define AUDIO_WAV_EVENT_BUFFER1_READY ((EventBits_t)(1U << 3U))
#define AUDIO_WAV_EVENT_STOP          ((EventBits_t)(1U << 4U))
#define AUDIO_WAV_EVENT_DONE          ((EventBits_t)(1U << 5U))

#define AUDIO_WAV_EVENT_ALL_FREE \
    (AUDIO_WAV_EVENT_BUFFER0_FREE | AUDIO_WAV_EVENT_BUFFER1_FREE)
#endif

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "AUDIO_WAV";

#ifdef ESP_PLATFORM
/* Type Definitions --------------------------------------------------------- */
typedef struct
{
    FILE *file;
    uint8_t *buffers[AUDIO_WAV_PREFETCH_BUFFER_COUNT];
    uint8_t *storage_staging;
    size_t filled_bytes[AUDIO_WAV_PREFETCH_BUFFER_COUNT];
    EventGroupHandle_t events;
    TaskHandle_t producer_task;

    uint32_t producer_bytes_remaining;
    uint8_t producer_index;
    uint8_t consumer_index;
    int8_t borrowed_index;
    esp_err_t producer_result;

    uint32_t storage_read_count;
    uint32_t max_storage_read_duration_us;
    uint32_t consumer_wait_count;
    uint32_t max_consumer_wait_duration_us;
} audio_wav_prefetch_context_t;
#endif

/* Function Prototypes ------------------------------------------------------ */
static uint16_t audio_wav_read_le16(const uint8_t *bytes);
static uint32_t audio_wav_read_le32(const uint8_t *bytes);
static esp_err_t audio_wav_read_exact(
    FILE *file,
    void *buffer,
    size_t bytes_to_read);
static esp_err_t audio_wav_seek_absolute(
    FILE *file,
    uint64_t offset);
static esp_err_t audio_wav_get_file_size(
    FILE *file,
    uint64_t *file_size);
static esp_err_t audio_wav_validate_format(audio_wav_info_t *info);
static esp_err_t audio_wav_open_error_from_errno(int error_number);

#ifdef ESP_PLATFORM
static EventBits_t audio_wav_free_bit(uint8_t index);
static EventBits_t audio_wav_ready_bit(uint8_t index);
static uint32_t audio_wav_bound_duration_us(int64_t duration_us);
static bool audio_wav_prefetch_stop_requested(
    const audio_wav_prefetch_context_t *context);
static esp_err_t audio_wav_prefetch_fill_buffer(
    audio_wav_prefetch_context_t *context,
    uint8_t index,
    size_t target_bytes);
static void audio_wav_prefetch_task(void *argument);
static void audio_wav_prefetch_free_context(
    audio_wav_prefetch_context_t *context);
static esp_err_t audio_wav_prefetch_wait_first_buffer(
    audio_wav_stream_t *stream);
static void audio_wav_prefetch_release_borrowed(
    audio_wav_prefetch_context_t *context);
static esp_err_t audio_wav_stream_open_target(
    audio_wav_stream_t *stream,
    FILE *file,
    const audio_wav_info_t *info,
    const char *path);
static esp_err_t audio_wav_stream_read_target(
    audio_wav_stream_t *stream,
    const uint8_t **buffer,
    size_t *bytes_read);
static esp_err_t audio_wav_stream_close_target(audio_wav_stream_t *stream);
#endif

/* Static Functions --------------------------------------------------------- */
static uint16_t audio_wav_read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8U));
}

static uint32_t audio_wav_read_le32(const uint8_t *bytes)
{
    return (uint32_t)((uint32_t)bytes[0] |
                      ((uint32_t)bytes[1] << 8U) |
                      ((uint32_t)bytes[2] << 16U) |
                      ((uint32_t)bytes[3] << 24U));
}

static esp_err_t audio_wav_read_exact(
    FILE *file,
    void *buffer,
    size_t bytes_to_read)
{
    if ((file == NULL) || (buffer == NULL) || (bytes_to_read == 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t bytes_read = fread(buffer, 1U, bytes_to_read, file);
    if (bytes_read == bytes_to_read)
    {
        return ESP_OK;
    }

    return ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
}

static esp_err_t audio_wav_seek_absolute(
    FILE *file,
    uint64_t offset)
{
    if (file == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (offset > (uint64_t)LONG_MAX)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fseek(file, (long)offset, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t audio_wav_get_file_size(
    FILE *file,
    uint64_t *file_size)
{
    if ((file == NULL) || (file_size == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(file, 0L, SEEK_END) != 0)
    {
        return ESP_FAIL;
    }

    const long end_offset = ftell(file);
    if (end_offset < 0L)
    {
        return (errno == EOVERFLOW) ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }

    if (fseek(file, 0L, SEEK_SET) != 0)
    {
        return ESP_FAIL;
    }

    *file_size = (uint64_t)end_offset;
    return ESP_OK;
}

bool audio_wav_path_is_valid(const char *path)
{
    if ((path == NULL) || (path[0] == '\0'))
    {
        return false;
    }

    const size_t mount_length = strlen(SD_MOUNT_POINT);
    if (strncmp(path, SD_MOUNT_POINT, mount_length) != 0)
    {
        return false;
    }

    return (path[mount_length] == '/') && (path[mount_length + 1U] != '\0');
}

static esp_err_t audio_wav_validate_format(audio_wav_info_t *info)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((info->audio_format != AUDIO_WAV_PCM_FORMAT) ||
        (info->channels != AUDIO_WAV_MONO_CHANNELS) ||
        (info->sample_rate_hz != AUDIO_WAV_SAMPLE_RATE_HZ) ||
        (info->bits_per_sample != AUDIO_WAV_BITS_PER_SAMPLE))
    {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if ((info->block_align != AUDIO_WAV_BLOCK_ALIGN_BYTES) ||
        (info->byte_rate != AUDIO_WAV_BYTE_RATE))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t audio_wav_open_error_from_errno(int error_number)
{
    if ((error_number == ENOENT) || (error_number == ENOTDIR))
    {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_FAIL;
}

#ifdef ESP_PLATFORM
static EventBits_t audio_wav_free_bit(uint8_t index)
{
    return (index == 0U)
        ? AUDIO_WAV_EVENT_BUFFER0_FREE
        : AUDIO_WAV_EVENT_BUFFER1_FREE;
}

static EventBits_t audio_wav_ready_bit(uint8_t index)
{
    return (index == 0U)
        ? AUDIO_WAV_EVENT_BUFFER0_READY
        : AUDIO_WAV_EVENT_BUFFER1_READY;
}

static uint32_t audio_wav_bound_duration_us(int64_t duration_us)
{
    if (duration_us <= 0)
    {
        return 0U;
    }

    return ((uint64_t)duration_us > UINT32_MAX)
        ? UINT32_MAX
        : (uint32_t)duration_us;
}

static bool audio_wav_prefetch_stop_requested(
    const audio_wav_prefetch_context_t *context)
{
    if ((context == NULL) || (context->events == NULL))
    {
        return true;
    }

    return (xEventGroupGetBits(context->events) & AUDIO_WAV_EVENT_STOP) != 0U;
}

static esp_err_t audio_wav_prefetch_fill_buffer(
    audio_wav_prefetch_context_t *context,
    uint8_t index,
    size_t target_bytes)
{
    if ((context == NULL) ||
        (context->file == NULL) ||
        (index >= AUDIO_WAV_PREFETCH_BUFFER_COUNT) ||
        (context->buffers[index] == NULL) ||
        (context->storage_staging == NULL) ||
        (target_bytes == 0U) ||
        (target_bytes > AUDIO_WAV_PREFETCH_BUFFER_BYTES))
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t filled = 0U;
    while (filled < target_bytes)
    {
        if (audio_wav_prefetch_stop_requested(context))
        {
            return ESP_ERR_INVALID_STATE;
        }

        if (!sd_card_manager_is_mounted())
        {
            return ESP_ERR_INVALID_STATE;
        }

        size_t read_bytes = target_bytes - filled;
        if (read_bytes > AUDIO_WAV_STORAGE_READ_BYTES)
        {
            read_bytes = AUDIO_WAV_STORAGE_READ_BYTES;
        }

        const int64_t read_start_us = esp_timer_get_time();
        const size_t received = fread(
            context->storage_staging,
            1U,
            read_bytes,
            context->file);
        const uint32_t duration_us = audio_wav_bound_duration_us(
            esp_timer_get_time() - read_start_us);

        ++context->storage_read_count;
        if (duration_us > context->max_storage_read_duration_us)
        {
            context->max_storage_read_duration_us = duration_us;
        }

        if (received != read_bytes)
        {
            return ferror(context->file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
        }

        memcpy(
            &context->buffers[index][filled],
            context->storage_staging,
            received);
        filled += received;
    }

    context->filled_bytes[index] = filled;
    return ESP_OK;
}

static void audio_wav_prefetch_task(void *argument)
{
    audio_wav_prefetch_context_t *const context =
        (audio_wav_prefetch_context_t *)argument;

    if ((context == NULL) || (context->events == NULL))
    {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t result = ESP_OK;

    while ((context->producer_bytes_remaining > 0U) &&
           !audio_wav_prefetch_stop_requested(context))
    {
        const uint8_t index = context->producer_index;
        const EventBits_t free_bit = audio_wav_free_bit(index);

        const EventBits_t bits = xEventGroupWaitBits(
            context->events,
            free_bit | AUDIO_WAV_EVENT_STOP,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

        if ((bits & AUDIO_WAV_EVENT_STOP) != 0U)
        {
            break;
        }

        if ((bits & free_bit) == 0U)
        {
            result = ESP_FAIL;
            break;
        }

        (void)xEventGroupClearBits(context->events, free_bit);

        size_t target_bytes = context->producer_bytes_remaining;
        if (target_bytes > AUDIO_WAV_PREFETCH_BUFFER_BYTES)
        {
            target_bytes = AUDIO_WAV_PREFETCH_BUFFER_BYTES;
        }

        result = audio_wav_prefetch_fill_buffer(context, index, target_bytes);
        if (result != ESP_OK)
        {
            if (audio_wav_prefetch_stop_requested(context))
            {
                result = ESP_OK;
            }
            break;
        }

        context->producer_bytes_remaining -= (uint32_t)target_bytes;
        (void)xEventGroupSetBits(context->events, audio_wav_ready_bit(index));
        context->producer_index ^= 1U;
    }

    context->producer_result = result;
    context->producer_task = NULL;
    (void)xEventGroupSetBits(context->events, AUDIO_WAV_EVENT_DONE);
    vTaskDelete(NULL);
}

static void audio_wav_prefetch_free_context(
    audio_wav_prefetch_context_t *context)
{
    if (context == NULL)
    {
        return;
    }

    if (context->events != NULL)
    {
        vEventGroupDelete(context->events);
        context->events = NULL;
    }

    heap_caps_free(context->storage_staging);
    context->storage_staging = NULL;

    for (uint8_t index = 0U;
         index < AUDIO_WAV_PREFETCH_BUFFER_COUNT;
         ++index)
    {
        heap_caps_free(context->buffers[index]);
        context->buffers[index] = NULL;
    }

    heap_caps_free(context);
}

static esp_err_t audio_wav_prefetch_wait_first_buffer(
    audio_wav_stream_t *stream)
{
    if ((stream == NULL) || (stream->prefetch_context == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    audio_wav_prefetch_context_t *const context =
        (audio_wav_prefetch_context_t *)stream->prefetch_context;

    const EventBits_t bits = xEventGroupWaitBits(
        context->events,
        AUDIO_WAV_EVENT_BUFFER0_READY | AUDIO_WAV_EVENT_DONE,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_START_TIMEOUT_MS));

    if ((bits & AUDIO_WAV_EVENT_BUFFER0_READY) != 0U)
    {
        return ESP_OK;
    }

    if ((bits & AUDIO_WAV_EVENT_DONE) != 0U)
    {
        return (context->producer_result != ESP_OK)
            ? context->producer_result
            : ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_ERR_TIMEOUT;
}

static void audio_wav_prefetch_release_borrowed(
    audio_wav_prefetch_context_t *context)
{
    if ((context == NULL) ||
        (context->events == NULL) ||
        (context->borrowed_index < 0))
    {
        return;
    }

    const uint8_t index = (uint8_t)context->borrowed_index;
    context->filled_bytes[index] = 0U;
    context->borrowed_index = -1;
    (void)xEventGroupSetBits(context->events, audio_wav_free_bit(index));
}

static esp_err_t audio_wav_stream_open_target(
    audio_wav_stream_t *stream,
    FILE *file,
    const audio_wav_info_t *info,
    const char *path)
{
    if ((stream == NULL) || (file == NULL) || (info == NULL) || (path == NULL))
    {
        if (file != NULL)
        {
            (void)fclose(file);
        }
        return ESP_ERR_INVALID_ARG;
    }

    audio_wav_prefetch_context_t *const context =
        (audio_wav_prefetch_context_t *)heap_caps_calloc(
            1U,
            sizeof(audio_wav_prefetch_context_t),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (context == NULL)
    {
        (void)fclose(file);
        return ESP_ERR_NO_MEM;
    }

    context->file = file;
    context->producer_bytes_remaining = info->data_size_bytes;
    context->producer_result = ESP_OK;
    context->borrowed_index = -1;

    for (uint8_t index = 0U;
         index < AUDIO_WAV_PREFETCH_BUFFER_COUNT;
         ++index)
    {
        context->buffers[index] = (uint8_t *)heap_caps_malloc(
            AUDIO_WAV_PREFETCH_BUFFER_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if ((context->buffers[index] == NULL) ||
            !esp_ptr_external_ram(context->buffers[index]))
        {
            audio_wav_prefetch_free_context(context);
            (void)fclose(file);
            return ESP_ERR_NO_MEM;
        }
    }

    context->storage_staging = (uint8_t *)heap_caps_malloc(
        AUDIO_WAV_STORAGE_READ_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (context->storage_staging == NULL)
    {
        audio_wav_prefetch_free_context(context);
        (void)fclose(file);
        return ESP_ERR_NO_MEM;
    }

    context->events = xEventGroupCreate();
    if (context->events == NULL)
    {
        audio_wav_prefetch_free_context(context);
        (void)fclose(file);
        return ESP_ERR_NO_MEM;
    }

    (void)xEventGroupSetBits(context->events, AUDIO_WAV_EVENT_ALL_FREE);

    audio_wav_stream_reset(stream);
    stream->file = file;
    stream->buffer = context->buffers[0];
    stream->info = *info;
    stream->data_bytes_remaining = info->data_size_bytes;
    stream->prefetch_context = context;

    const BaseType_t task_result = xTaskCreate(
        audio_wav_prefetch_task,
        AUDIO_WAV_PREFETCH_TASK_NAME,
        AUDIO_WAV_PREFETCH_TASK_STACK_SIZE,
        context,
        AUDIO_WAV_PREFETCH_TASK_PRIORITY,
        &context->producer_task);
    if (task_result != pdPASS)
    {
        stream->file = NULL;
        stream->buffer = NULL;
        stream->prefetch_context = NULL;
        audio_wav_prefetch_free_context(context);
        (void)fclose(file);
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t ready_result = audio_wav_prefetch_wait_first_buffer(stream);
    if (ready_result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Initial WAV prefetch failed path=%s error=%s",
            path,
            esp_err_to_name(ready_result));
        const esp_err_t close_result = audio_wav_stream_close_target(stream);
        if (close_result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Cleanup after initial prefetch failure: %s",
                esp_err_to_name(close_result));
        }
        return ready_result;
    }

    ESP_LOGI(
        TAG,
        "WAV ping-pong prefetch ready: buffers=2 each=%uB (~%us) total_psram=%uB sd_stage=%uB path=%s",
        (unsigned)AUDIO_WAV_PREFETCH_BUFFER_BYTES,
        (unsigned)AUDIO_WAV_PREFETCH_SECONDS,
        (unsigned)(AUDIO_WAV_PREFETCH_BUFFER_BYTES *
                   AUDIO_WAV_PREFETCH_BUFFER_COUNT),
        (unsigned)AUDIO_WAV_STORAGE_READ_BYTES,
        path);

    return ESP_OK;
}

static esp_err_t audio_wav_stream_read_target(
    audio_wav_stream_t *stream,
    const uint8_t **buffer,
    size_t *bytes_read)
{
    audio_wav_prefetch_context_t *const context =
        (audio_wav_prefetch_context_t *)stream->prefetch_context;
    if ((context == NULL) || (context->events == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_wav_prefetch_release_borrowed(context);

    if (stream->data_bytes_remaining == 0U)
    {
        return ESP_OK;
    }

    const uint8_t index = context->consumer_index;
    const EventBits_t ready_bit = audio_wav_ready_bit(index);
    const EventBits_t before_bits = xEventGroupGetBits(context->events);
    const bool was_ready = (before_bits & ready_bit) != 0U;
    const int64_t wait_start_us = esp_timer_get_time();

    const EventBits_t bits = xEventGroupWaitBits(
        context->events,
        ready_bit | AUDIO_WAV_EVENT_DONE,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_WAIT_TIMEOUT_MS));

    const uint32_t wait_duration_us = audio_wav_bound_duration_us(
        esp_timer_get_time() - wait_start_us);

    if (!was_ready)
    {
        ++context->consumer_wait_count;
        if (wait_duration_us > context->max_consumer_wait_duration_us)
        {
            context->max_consumer_wait_duration_us = wait_duration_us;
        }

        if (wait_duration_us >= AUDIO_WAV_PREFETCH_STALL_WARN_US)
        {
            ESP_LOGW(
                TAG,
                "WAV prefetch starvation: buffer=%u wait=%uus",
                (unsigned)index,
                (unsigned)wait_duration_us);
        }
    }

    if ((bits & ready_bit) == 0U)
    {
        if ((bits & AUDIO_WAV_EVENT_DONE) != 0U)
        {
            return (context->producer_result != ESP_OK)
                ? context->producer_result
                : ESP_ERR_INVALID_SIZE;
        }

        return ESP_ERR_TIMEOUT;
    }

    (void)xEventGroupClearBits(context->events, ready_bit);

    const size_t available_bytes = context->filled_bytes[index];
    if ((available_bytes == 0U) ||
        (available_bytes > stream->data_bytes_remaining) ||
        ((available_bytes % AUDIO_WAV_BLOCK_ALIGN_BYTES) != 0U))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    context->borrowed_index = (int8_t)index;
    context->consumer_index ^= 1U;

    stream->data_bytes_remaining -= (uint32_t)available_bytes;
    stream->data_bytes_read += available_bytes;
    stream->buffer = context->buffers[index];
    *buffer = context->buffers[index];
    *bytes_read = available_bytes;
    return ESP_OK;
}

static esp_err_t audio_wav_stream_close_target(audio_wav_stream_t *stream)
{
    audio_wav_prefetch_context_t *const context =
        (audio_wav_prefetch_context_t *)stream->prefetch_context;

    if (context == NULL)
    {
        esp_err_t result = ESP_OK;
        if ((stream->file != NULL) && (fclose(stream->file) != 0))
        {
            result = ESP_FAIL;
        }
        audio_wav_stream_reset(stream);
        return result;
    }

    (void)xEventGroupSetBits(context->events, AUDIO_WAV_EVENT_STOP);

    const EventBits_t bits = xEventGroupWaitBits(
        context->events,
        AUDIO_WAV_EVENT_DONE,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_STOP_TIMEOUT_MS));

    if ((bits & AUDIO_WAV_EVENT_DONE) == 0U)
    {
        ESP_LOGE(
            TAG,
            "WAV prefetch task did not stop within %ums; resources retained",
            (unsigned)AUDIO_WAV_PREFETCH_STOP_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(
        TAG,
        "WAV_PREFETCH_DIAG storage_reads=%u max_fread_us=%u consumer_waits=%u max_wait_us=%u",
        (unsigned)context->storage_read_count,
        (unsigned)context->max_storage_read_duration_us,
        (unsigned)context->consumer_wait_count,
        (unsigned)context->max_consumer_wait_duration_us);

    esp_err_t result = context->producer_result;
    if ((result == ESP_ERR_INVALID_STATE) &&
        ((xEventGroupGetBits(context->events) & AUDIO_WAV_EVENT_STOP) != 0U))
    {
        result = ESP_OK;
    }

    if ((stream->file != NULL) && (fclose(stream->file) != 0))
    {
        ESP_LOGW(TAG, "Failed to close WAV file: errno=%d", errno);
        if (result == ESP_OK)
        {
            result = ESP_FAIL;
        }
    }

    audio_wav_prefetch_free_context(context);
    audio_wav_stream_reset(stream);
    return result;
}
#endif

/* Functions ---------------------------------------------------------------- */
void audio_wav_stream_reset(audio_wav_stream_t *stream)
{
    if (stream != NULL)
    {
        memset(stream, 0, sizeof(*stream));
    }
}

esp_err_t audio_wav_parse_file(
    FILE *file,
    audio_wav_info_t *info)
{
    if ((file == NULL) || (info == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *info = (audio_wav_info_t) {0};

    uint64_t file_size = 0U;
    esp_err_t result = audio_wav_get_file_size(file, &file_size);
    if (result != ESP_OK)
    {
        return result;
    }

    if (file_size < AUDIO_WAV_RIFF_HEADER_BYTES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t riff_header[AUDIO_WAV_RIFF_HEADER_BYTES] = {0};
    result = audio_wav_read_exact(file, riff_header, sizeof(riff_header));
    if (result != ESP_OK)
    {
        return result;
    }

    if ((memcmp(riff_header, "RIFF", 4U) != 0) ||
        (memcmp(&riff_header[8], "WAVE", 4U) != 0))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t riff_size = audio_wav_read_le32(&riff_header[4]);
    if (riff_size < 4U)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint64_t riff_end_offset = 8U + (uint64_t)riff_size;
    if ((riff_end_offset < AUDIO_WAV_RIFF_HEADER_BYTES) ||
        (riff_end_offset > file_size))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    bool found_format = false;
    bool found_data = false;
    uint64_t chunk_offset = AUDIO_WAV_RIFF_HEADER_BYTES;

    while (chunk_offset < riff_end_offset)
    {
        if ((riff_end_offset - chunk_offset) < AUDIO_WAV_CHUNK_HEADER_BYTES)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        uint8_t chunk_header[AUDIO_WAV_CHUNK_HEADER_BYTES] = {0};
        result = audio_wav_read_exact(file, chunk_header, sizeof(chunk_header));
        if (result != ESP_OK)
        {
            return result;
        }

        const uint32_t chunk_size = audio_wav_read_le32(&chunk_header[4]);
        const uint64_t chunk_data_offset =
            chunk_offset + AUDIO_WAV_CHUNK_HEADER_BYTES;
        const uint64_t chunk_padded_size =
            (uint64_t)chunk_size + (uint64_t)(chunk_size & 1U);
        const uint64_t next_chunk_offset =
            chunk_data_offset + chunk_padded_size;

        if ((next_chunk_offset < chunk_data_offset) ||
            (next_chunk_offset > riff_end_offset) ||
            (next_chunk_offset > file_size))
        {
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk_header, "fmt ", 4U) == 0)
        {
            if (found_format || (chunk_size < AUDIO_WAV_FMT_MIN_BYTES))
            {
                return ESP_ERR_INVALID_RESPONSE;
            }

            uint8_t format_bytes[AUDIO_WAV_FMT_MIN_BYTES] = {0};
            result = audio_wav_read_exact(
                file,
                format_bytes,
                sizeof(format_bytes));
            if (result != ESP_OK)
            {
                return result;
            }

            info->audio_format = audio_wav_read_le16(&format_bytes[0]);
            info->channels = audio_wav_read_le16(&format_bytes[2]);
            info->sample_rate_hz = audio_wav_read_le32(&format_bytes[4]);
            info->byte_rate = audio_wav_read_le32(&format_bytes[8]);
            info->block_align = audio_wav_read_le16(&format_bytes[12]);
            info->bits_per_sample = audio_wav_read_le16(&format_bytes[14]);

            result = audio_wav_validate_format(info);
            if (result != ESP_OK)
            {
                return result;
            }

            found_format = true;
        }
        else if (memcmp(chunk_header, "data", 4U) == 0)
        {
            if (found_data || (chunk_size == 0U))
            {
                return ESP_ERR_INVALID_SIZE;
            }

            if (chunk_data_offset > (uint64_t)LONG_MAX)
            {
                return ESP_ERR_NOT_SUPPORTED;
            }

            info->data_offset = (long)chunk_data_offset;
            info->data_size_bytes = chunk_size;
            found_data = true;
        }

        result = audio_wav_seek_absolute(file, next_chunk_offset);
        if (result != ESP_OK)
        {
            return result;
        }

        chunk_offset = next_chunk_offset;

        if (found_format && found_data)
        {
            break;
        }
    }

    if (!found_format || !found_data)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((info->data_size_bytes == 0U) ||
        ((info->data_size_bytes % info->block_align) != 0U))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    info->duration_ms = (uint32_t)(
        ((uint64_t)info->data_size_bytes * 1000U) / info->byte_rate);

    return audio_wav_seek_absolute(file, (uint64_t)info->data_offset);
}

esp_err_t audio_wav_stream_open(
    audio_wav_stream_t *stream,
    const char *path)
{
    if ((stream == NULL) || !audio_wav_path_is_valid(path))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((stream->file != NULL) ||
        (stream->buffer != NULL) ||
        (stream->prefetch_context != NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!sd_card_manager_is_mounted())
    {
        ESP_LOGW(TAG, "SD filesystem is not mounted for WAV path: %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        const int open_errno = errno;
        ESP_LOGW(TAG, "Failed to open WAV %s: errno=%d", path, open_errno);
        return audio_wav_open_error_from_errno(open_errno);
    }

    if (setvbuf(file, NULL, _IONBF, 0) != 0)
    {
        ESP_LOGW(TAG, "Failed to configure unbuffered WAV I/O: %s", path);
        (void)fclose(file);
        return ESP_FAIL;
    }

    audio_wav_info_t info = {0};
    esp_err_t result = audio_wav_parse_file(file, &info);
    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Rejected WAV %s: %s",
            path,
            esp_err_to_name(result));
        (void)fclose(file);
        return result;
    }

#ifdef ESP_PLATFORM
    /* Target helper takes ownership of FILE on entry, including every failure. */
    result = audio_wav_stream_open_target(stream, file, &info, path);
    if (result != ESP_OK)
    {
        return result;
    }
#else
    uint8_t *const buffer = (uint8_t *)heap_caps_malloc(
        AUDIO_WAV_STREAM_BUFFER_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL)
    {
        (void)fclose(file);
        return ESP_ERR_NO_MEM;
    }

    audio_wav_stream_reset(stream);
    stream->file = file;
    stream->buffer = buffer;
    stream->info = info;
    stream->data_bytes_remaining = info.data_size_bytes;
#endif

    ESP_LOGI(
        TAG,
        "WAV opened path=%s fmt=%u ch=%u rate=%u bits=%u data_offset=%ld data_bytes=%u duration=%ums",
        path,
        (unsigned)info.audio_format,
        (unsigned)info.channels,
        (unsigned)info.sample_rate_hz,
        (unsigned)info.bits_per_sample,
        info.data_offset,
        (unsigned)info.data_size_bytes,
        (unsigned)info.duration_ms);

    return ESP_OK;
}

esp_err_t audio_wav_stream_read(
    audio_wav_stream_t *stream,
    const uint8_t **buffer,
    size_t *bytes_read)
{
    if ((stream == NULL) || (buffer == NULL) || (bytes_read == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    *buffer = NULL;
    *bytes_read = 0U;

    if ((stream->file == NULL) || (stream->buffer == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

#ifdef ESP_PLATFORM
    const esp_err_t target_result = audio_wav_stream_read_target(
        stream,
        buffer,
        bytes_read);
    if (target_result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "WAV prefetch read failed: %s",
            esp_err_to_name(target_result));
        const esp_err_t close_result = audio_wav_stream_close(stream);
        if (close_result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "WAV cleanup after prefetch read failure failed: %s",
                esp_err_to_name(close_result));
        }
    }
    return target_result;
#else
    if (!sd_card_manager_is_mounted())
    {
        const esp_err_t close_result = audio_wav_stream_close(stream);
        (void)close_result;
        return ESP_ERR_INVALID_STATE;
    }

    if (stream->data_bytes_remaining == 0U)
    {
        return ESP_OK;
    }

    size_t requested_bytes = stream->data_bytes_remaining;
    if (requested_bytes > AUDIO_WAV_STREAM_BUFFER_BYTES)
    {
        requested_bytes = AUDIO_WAV_STREAM_BUFFER_BYTES;
    }

    const size_t received_bytes = fread(
        stream->buffer,
        1U,
        requested_bytes,
        stream->file);
    if (received_bytes != requested_bytes)
    {
        const esp_err_t result = ferror(stream->file)
                                     ? ESP_FAIL
                                     : ESP_ERR_INVALID_SIZE;
        const esp_err_t close_result = audio_wav_stream_close(stream);
        (void)close_result;
        return result;
    }

    stream->data_bytes_remaining -= (uint32_t)received_bytes;
    stream->data_bytes_read += received_bytes;
    *buffer = stream->buffer;
    *bytes_read = received_bytes;
    return ESP_OK;
#endif
}

esp_err_t audio_wav_stream_close(audio_wav_stream_t *stream)
{
    if (stream == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef ESP_PLATFORM
    if (stream->prefetch_context != NULL)
    {
        return audio_wav_stream_close_target(stream);
    }
#endif

    esp_err_t result = ESP_OK;
    if ((stream->file != NULL) && (fclose(stream->file) != 0))
    {
        ESP_LOGW(TAG, "Failed to close WAV file: errno=%d", errno);
        result = ESP_FAIL;
    }

    heap_caps_free(stream->buffer);
    audio_wav_stream_reset(stream);
    return result;
}
