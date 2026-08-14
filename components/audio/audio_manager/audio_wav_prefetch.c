/**
 * @file audio_wav_prefetch.c
 * @brief Private ping-pong SD/WAV prefetch implementation.
 */

/* Includes ----------------------------------------------------------------- */
#include "audio_wav_prefetch.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "sd_card_manager.h"

/* Macros ------------------------------------------------------------------- */
#define AUDIO_WAV_PREFETCH_TASK_NAME             "wav_prefetch"
#define AUDIO_WAV_PREFETCH_TASK_STACK_SIZE       4096U
#define AUDIO_WAV_PREFETCH_QUEUE_LENGTH          AUDIO_WAV_PREFETCH_SLOT_COUNT
#define AUDIO_WAV_PREFETCH_WORKER_POLL_MS         100U
#define AUDIO_WAV_PREFETCH_RECOVERY_TIMEOUT_MS   5000U
#define AUDIO_WAV_PREFETCH_MAX_RECOVERY_ATTEMPTS    1U
#define AUDIO_WAV_PREFETCH_WORKER_STOPPED_BIT ((EventBits_t)(1U << 0U))
#define AUDIO_WAV_PREFETCH_WORKER_DESTROY_ACK_BIT ((EventBits_t)(1U << 1U))

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "AUDIO_WAV_PREFETCH";

/* Function Prototypes ------------------------------------------------------ */
static bool audio_wav_prefetch_stop_is_requested(
    audio_wav_prefetch_t *prefetch);
static void audio_wav_prefetch_set_worker_terminal(
    audio_wav_prefetch_t *prefetch,
    esp_err_t result);
static uint32_t audio_wav_prefetch_bound_duration_us(int64_t duration_us);
static uint32_t audio_wav_prefetch_bound_duration_ms(int64_t duration_us);
static bool audio_wav_prefetch_info_matches(
    const audio_wav_info_t *expected,
    const audio_wav_info_t *actual);
static esp_err_t audio_wav_prefetch_resume_after_recovery(
    audio_wav_prefetch_t *prefetch,
    uint64_t committed_data_offset);
static bool audio_wav_prefetch_publish_item(
    audio_wav_prefetch_t *prefetch,
    const audio_wav_prefetch_item_t *item);
static void audio_wav_prefetch_free_resources(
    audio_wav_prefetch_t *prefetch);
static void audio_wav_prefetch_task(void *argument);

/* Static Functions --------------------------------------------------------- */
static bool audio_wav_prefetch_stop_is_requested(
    audio_wav_prefetch_t *prefetch)
{
    bool stop_requested;

    portENTER_CRITICAL(&prefetch->lock);
    stop_requested = prefetch->stop_requested;
    portEXIT_CRITICAL(&prefetch->lock);

    return stop_requested;
}

static void audio_wav_prefetch_set_worker_terminal(
    audio_wav_prefetch_t *prefetch,
    esp_err_t result)
{
    portENTER_CRITICAL(&prefetch->lock);
    prefetch->worker_result = result;
    prefetch->worker_stopped = true;
    portEXIT_CRITICAL(&prefetch->lock);
}

static uint32_t audio_wav_prefetch_bound_duration_us(int64_t duration_us)
{
    if (duration_us <= 0)
    {
        return 0U;
    }

    return ((uint64_t)duration_us > UINT32_MAX)
               ? UINT32_MAX
               : (uint32_t)duration_us;
}

static uint32_t audio_wav_prefetch_bound_duration_ms(int64_t duration_us)
{
    if (duration_us <= 0)
    {
        return 0U;
    }

    const uint64_t duration_ms = (uint64_t)duration_us / 1000U;
    return (duration_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)duration_ms;
}

static bool audio_wav_prefetch_info_matches(
    const audio_wav_info_t *expected,
    const audio_wav_info_t *actual)
{
    return (expected != NULL) && (actual != NULL) &&
           (expected->audio_format == actual->audio_format) &&
           (expected->channels == actual->channels) &&
           (expected->sample_rate_hz == actual->sample_rate_hz) &&
           (expected->byte_rate == actual->byte_rate) &&
           (expected->block_align == actual->block_align) &&
           (expected->bits_per_sample == actual->bits_per_sample) &&
           (expected->data_size_bytes == actual->data_size_bytes) &&
           (expected->data_offset == actual->data_offset) &&
           (expected->duration_ms == actual->duration_ms);
}

/**
 * @brief Resume from the last committed offset with a fresh FILE.
 *
 * The failed FILE is already closed and its SD lease is released. The first
 * retry deliberately happens while the current mounted VFS is still READY, so
 * one transient fread() error does not force an SD card reinitialization. If
 * the fresh open/seek confirms a media fault and moves sd_card_manager into
 * RECOVERING, this same bounded loop waits for remount and retries with another
 * fresh FILE. The logical recovery-attempt budget remains exactly one.
 */
static esp_err_t audio_wav_prefetch_resume_after_recovery(
    audio_wav_prefetch_t *prefetch,
    uint64_t committed_data_offset)
{
    if ((prefetch == NULL) || !prefetch->info_valid ||
        (prefetch->metrics.recovery_attempt_count >=
         AUDIO_WAV_PREFETCH_MAX_RECOVERY_ATTEMPTS))
    {
        return ESP_ERR_INVALID_STATE;
    }

    ++prefetch->metrics.recovery_attempt_count;
    prefetch->metrics.last_recovery_data_offset = committed_data_offset;
    const int64_t recovery_start_us = esp_timer_get_time();

    ESP_LOGW(
        TAG,
        "WAV retry at data offset %llu: fresh reopen first, SD remount fallback if required",
        (unsigned long long)committed_data_offset);

    while (!audio_wav_prefetch_stop_is_requested(prefetch))
    {
        const uint32_t elapsed_ms = audio_wav_prefetch_bound_duration_ms(
            esp_timer_get_time() - recovery_start_us);
        if (elapsed_ms >= AUDIO_WAV_PREFETCH_RECOVERY_TIMEOUT_MS)
        {
            prefetch->metrics.recovery_wait_ms = elapsed_ms;
            ESP_LOGW(
                TAG,
                "WAV resume timed out after %ums at data offset %llu",
                (unsigned)elapsed_ms,
                (unsigned long long)committed_data_offset);
            return ESP_ERR_TIMEOUT;
        }

        if (!sd_card_manager_is_mounted())
        {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_WORKER_POLL_MS));
            continue;
        }

        esp_err_t result = audio_wav_stream_open(
            &prefetch->stream,
            prefetch->path);
        if (result == ESP_ERR_INVALID_STATE)
        {
            /* READY can briefly change while acquire races an idle probe. */
            vTaskDelay(pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_WORKER_POLL_MS));
            continue;
        }
        if (result != ESP_OK)
        {
            prefetch->metrics.recovery_wait_ms =
                audio_wav_prefetch_bound_duration_ms(
                    esp_timer_get_time() - recovery_start_us);

            /*
             * audio_wav_stream_open() reports confirmed VFS/media failures to
             * sd_card_manager. If that transitioned the card out of READY,
             * wait for the existing remount recovery instead of terminating
             * this logical retry immediately.
             */
            if ((result == ESP_FAIL) && !sd_card_manager_is_mounted())
            {
                ESP_LOGW(
                    TAG,
                    "Fresh WAV reopen confirmed media fault; waiting for SD remount");
                vTaskDelay(pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_WORKER_POLL_MS));
                continue;
            }

            return result;
        }

        prefetch->metrics.recovery_wait_ms =
            audio_wav_prefetch_bound_duration_ms(
                esp_timer_get_time() - recovery_start_us);

        if (!audio_wav_prefetch_info_matches(
                &prefetch->info,
                &prefetch->stream.info))
        {
            ESP_LOGE(
                TAG,
                "WAV changed while retrying; refusing unsafe resume");
            const esp_err_t close_result =
                audio_wav_stream_close(&prefetch->stream);
            if (close_result != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "WAV close after metadata mismatch failed: %s",
                    esp_err_to_name(close_result));
            }
            return ESP_ERR_INVALID_RESPONSE;
        }

        result = audio_wav_stream_seek_data(
            &prefetch->stream,
            committed_data_offset);
        if (result != ESP_OK)
        {
            const esp_err_t close_result =
                audio_wav_stream_close(&prefetch->stream);
            if (close_result != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "WAV close after resume seek failed: %s",
                    esp_err_to_name(close_result));
            }

            if ((result == ESP_FAIL) && !sd_card_manager_is_mounted())
            {
                ESP_LOGW(
                    TAG,
                    "Fresh WAV seek confirmed media fault; waiting for SD remount");
                vTaskDelay(pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_WORKER_POLL_MS));
                continue;
            }

            return result;
        }

        prefetch->metrics.recovery_wait_ms =
            audio_wav_prefetch_bound_duration_ms(
                esp_timer_get_time() - recovery_start_us);
        ++prefetch->metrics.recovery_success_count;
        ESP_LOGI(
            TAG,
            "WAV resumed with a fresh file at data offset %llu after %ums",
            (unsigned long long)committed_data_offset,
            (unsigned)prefetch->metrics.recovery_wait_ms);
        return ESP_OK;
    }

    prefetch->metrics.recovery_wait_ms =
        audio_wav_prefetch_bound_duration_ms(
            esp_timer_get_time() - recovery_start_us);
    return ESP_ERR_INVALID_STATE;
}

static bool audio_wav_prefetch_publish_item(
    audio_wav_prefetch_t *prefetch,
    const audio_wav_prefetch_item_t *item)
{
    while (!audio_wav_prefetch_stop_is_requested(prefetch))
    {
        if (xQueueSend(
                prefetch->ready_items,
                item,
                pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_WORKER_POLL_MS)) == pdTRUE)
        {
            return true;
        }
    }

    return false;
}

static void audio_wav_prefetch_free_resources(
    audio_wav_prefetch_t *prefetch)
{
    for (size_t index = 0U;
         index < AUDIO_WAV_PREFETCH_SLOT_COUNT;
         ++index)
    {
        heap_caps_free(prefetch->slots[index].data);
        prefetch->slots[index].data = NULL;
    }

    if (prefetch->free_slots != NULL)
    {
        vQueueDelete(prefetch->free_slots);
        prefetch->free_slots = NULL;
    }

    if (prefetch->ready_items != NULL)
    {
        vQueueDelete(prefetch->ready_items);
        prefetch->ready_items = NULL;
    }

    if (prefetch->events != NULL)
    {
        vEventGroupDelete(prefetch->events);
        prefetch->events = NULL;
    }
}

/**
 * @brief Fill one logical PSRAM slot from bounded raw WAV reads.
 *
 * The worker deliberately reuses audio_wav's small Internal staging buffer.
 * It can consequently check a stop request between raw reads while keeping
 * the consumer task entirely out of VFS/FATFS.
 */
static void audio_wav_prefetch_task(void *argument)
{
    audio_wav_prefetch_t *const prefetch =
        (audio_wav_prefetch_t *)argument;
    esp_err_t result = ESP_OK;

    if ((prefetch == NULL) || audio_wav_prefetch_stop_is_requested(prefetch))
    {
        result = ESP_OK;
    }
    else
    {
        result = audio_wav_stream_open(&prefetch->stream, prefetch->path);
        if (result == ESP_OK)
        {
            prefetch->info = prefetch->stream.info;
            prefetch->info_valid = true;
        }
    }

    while ((result == ESP_OK) &&
           !audio_wav_prefetch_stop_is_requested(prefetch) &&
           (prefetch->stream.data_bytes_remaining > 0U))
    {
        uint8_t slot_index = AUDIO_WAV_PREFETCH_INVALID_SLOT;
        if (xQueueReceive(
                prefetch->free_slots,
                &slot_index,
                pdMS_TO_TICKS(AUDIO_WAV_PREFETCH_WORKER_POLL_MS)) != pdTRUE)
        {
            continue;
        }

        if ((slot_index >= AUDIO_WAV_PREFETCH_SLOT_COUNT) ||
            (prefetch->slots[slot_index].data == NULL))
        {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        size_t bytes_filled = 0U;
        const int64_t fill_start_us = esp_timer_get_time();

        while ((bytes_filled < prefetch->slot_bytes) &&
               (prefetch->stream.data_bytes_remaining > 0U) &&
               !audio_wav_prefetch_stop_is_requested(prefetch))
        {
            const uint8_t *raw_bytes = NULL;
            size_t raw_bytes_read = 0U;
            const int64_t read_start_us = esp_timer_get_time();
            const esp_err_t read_result = audio_wav_stream_read_limited(
                &prefetch->stream,
                prefetch->slot_bytes - bytes_filled,
                &raw_bytes,
                &raw_bytes_read);
            const uint32_t read_duration_us =
                audio_wav_prefetch_bound_duration_us(
                    esp_timer_get_time() - read_start_us);

            ++prefetch->metrics.io_read_count;
            if (read_duration_us > prefetch->metrics.max_io_read_duration_us)
            {
                prefetch->metrics.max_io_read_duration_us = read_duration_us;
            }

            if (read_result != ESP_OK)
            {
                ++prefetch->metrics.io_read_failure_count;
                const bool recovery_allowed =
                    ((read_result == ESP_FAIL) ||
                     (read_result == ESP_ERR_INVALID_STATE)) &&
                    (prefetch->metrics.recovery_attempt_count <
                     AUDIO_WAV_PREFETCH_MAX_RECOVERY_ATTEMPTS);
                if (recovery_allowed)
                {
                    const esp_err_t recovery_result =
                        audio_wav_prefetch_resume_after_recovery(
                            prefetch,
                            prefetch->metrics.data_bytes_read);
                    if (recovery_result == ESP_OK)
                    {
                        continue;
                    }
                    result = recovery_result;
                }
                else
                {
                    result = read_result;
                }
                break;
            }

            if ((raw_bytes == NULL) || (raw_bytes_read == 0U) ||
                (raw_bytes_read > (prefetch->slot_bytes - bytes_filled)) ||
                ((raw_bytes_read % sizeof(int16_t)) != 0U))
            {
                ++prefetch->metrics.io_read_failure_count;
                result = ESP_ERR_INVALID_SIZE;
                break;
            }

            memcpy(
                &prefetch->slots[slot_index].data[bytes_filled],
                raw_bytes,
                raw_bytes_read);
            bytes_filled += raw_bytes_read;
            prefetch->metrics.data_bytes_read += raw_bytes_read;
        }

        const uint32_t fill_duration_us =
            audio_wav_prefetch_bound_duration_us(
                esp_timer_get_time() - fill_start_us);
        if (fill_duration_us > prefetch->metrics.max_fill_duration_us)
        {
            prefetch->metrics.max_fill_duration_us = fill_duration_us;
        }

        if (audio_wav_prefetch_stop_is_requested(prefetch))
        {
            break;
        }

        if (result != ESP_OK)
        {
            ++prefetch->metrics.fill_failure_count;
            break;
        }

        if (bytes_filled == 0U)
        {
            ++prefetch->metrics.fill_failure_count;
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        const audio_wav_prefetch_item_t item = {
            .slot_index = slot_index,
            .valid_bytes = bytes_filled,
            .final_block = (prefetch->stream.data_bytes_remaining == 0U),
            .result = ESP_OK,
        };
        ++prefetch->metrics.blocks_filled;

        if (!audio_wav_prefetch_publish_item(prefetch, &item))
        {
            break;
        }

        if (item.final_block)
        {
            break;
        }
    }

    const bool stopped_by_request = audio_wav_prefetch_stop_is_requested(prefetch);
    if ((result != ESP_OK) && !stopped_by_request)
    {
        const audio_wav_prefetch_item_t error_item = {
            .slot_index = AUDIO_WAV_PREFETCH_INVALID_SLOT,
            .valid_bytes = 0U,
            .final_block = true,
            .result = result,
        };
        (void)audio_wav_prefetch_publish_item(prefetch, &error_item);
    }

    const esp_err_t close_result = audio_wav_stream_close(&prefetch->stream);
    if ((result == ESP_OK) && !stopped_by_request && (close_result != ESP_OK))
    {
        result = close_result;
    }

    prefetch->metrics.task_stack_high_water = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(
        TAG,
        "reader stopped result=%s reads=%u blocks=%u max_fill_us=%u stack_hwm=%u",
        esp_err_to_name(stopped_by_request ? ESP_OK : result),
        (unsigned)prefetch->metrics.io_read_count,
        (unsigned)prefetch->metrics.blocks_filled,
        (unsigned)prefetch->metrics.max_fill_duration_us,
        (unsigned)prefetch->metrics.task_stack_high_water);

    audio_wav_prefetch_set_worker_terminal(
        prefetch,
        stopped_by_request ? ESP_OK : result);
    xEventGroupSetBits(
        prefetch->events,
        AUDIO_WAV_PREFETCH_WORKER_STOPPED_BIT);

    /*
     * Do not let the manager delete events until this task has returned from
     * xEventGroupSetBits().  It acknowledges the STOPPED bit, then this task
     * notifies the manager only after its final EventGroup access is complete.
     */
    const TaskHandle_t owner_task = prefetch->owner_task;
    (void)xEventGroupWaitBits(
        prefetch->events,
        AUDIO_WAV_PREFETCH_WORKER_DESTROY_ACK_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);
    if (owner_task != NULL)
    {
        xTaskNotifyGive(owner_task);
    }
    vTaskDelete(NULL);
}

/* Public Functions --------------------------------------------------------- */
esp_err_t audio_wav_prefetch_start(
    audio_wav_prefetch_t *prefetch,
    const char *path,
    size_t slot_bytes,
    UBaseType_t task_priority)
{
    if ((prefetch == NULL) || (path == NULL) || (slot_bytes == 0U) ||
        ((slot_bytes % sizeof(int16_t)) != 0U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t path_length = strnlen(path, AUDIO_MANAGER_WAV_PATH_MAX_BYTES);
    if ((path_length == 0U) ||
        (path_length >= AUDIO_MANAGER_WAV_PATH_MAX_BYTES) ||
        !audio_wav_path_is_valid(path) ||
        audio_wav_prefetch_is_active(prefetch))
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(prefetch, 0, sizeof(*prefetch));
    portMUX_INITIALIZE(&prefetch->lock);
    memcpy(prefetch->path, path, path_length + 1U);
    prefetch->slot_bytes = slot_bytes;
    prefetch->worker_result = ESP_OK;
    prefetch->owner_task = xTaskGetCurrentTaskHandle();

    prefetch->free_slots = xQueueCreate(
        AUDIO_WAV_PREFETCH_QUEUE_LENGTH,
        sizeof(uint8_t));
    prefetch->ready_items = xQueueCreate(
        AUDIO_WAV_PREFETCH_QUEUE_LENGTH,
        sizeof(audio_wav_prefetch_item_t));
    prefetch->events = xEventGroupCreate();
    if ((prefetch->free_slots == NULL) ||
        (prefetch->ready_items == NULL) ||
        (prefetch->events == NULL))
    {
        audio_wav_prefetch_free_resources(prefetch);
        memset(prefetch, 0, sizeof(*prefetch));
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t index = 0U;
         index < AUDIO_WAV_PREFETCH_SLOT_COUNT;
         ++index)
    {
        prefetch->slots[index].data = (uint8_t *)heap_caps_malloc(
            slot_bytes,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if ((prefetch->slots[index].data == NULL) ||
            !esp_ptr_external_ram(prefetch->slots[index].data))
        {
            audio_wav_prefetch_free_resources(prefetch);
            memset(prefetch, 0, sizeof(*prefetch));
            return ESP_ERR_NO_MEM;
        }

        if (xQueueSend(prefetch->free_slots, &index, 0U) != pdTRUE)
        {
            audio_wav_prefetch_free_resources(prefetch);
            memset(prefetch, 0, sizeof(*prefetch));
            return ESP_FAIL;
        }
    }

    prefetch->started = true;
    if (xTaskCreate(
            audio_wav_prefetch_task,
            AUDIO_WAV_PREFETCH_TASK_NAME,
            AUDIO_WAV_PREFETCH_TASK_STACK_SIZE,
            prefetch,
            task_priority,
            &prefetch->task_handle) != pdPASS)
    {
        prefetch->started = false;
        audio_wav_prefetch_free_resources(prefetch);
        memset(prefetch, 0, sizeof(*prefetch));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "started slot_bytes=%u slots=%u priority=%u path=%s",
        (unsigned)slot_bytes,
        (unsigned)AUDIO_WAV_PREFETCH_SLOT_COUNT,
        (unsigned)task_priority,
        path);
    return ESP_OK;
}

BaseType_t audio_wav_prefetch_take_ready(
    audio_wav_prefetch_t *prefetch,
    audio_wav_prefetch_item_t *item,
    TickType_t timeout)
{
    if ((prefetch == NULL) || (item == NULL) ||
        (prefetch->ready_items == NULL))
    {
        return pdFALSE;
    }

    return xQueueReceive(prefetch->ready_items, item, timeout);
}

esp_err_t audio_wav_prefetch_release_slot(
    audio_wav_prefetch_t *prefetch,
    uint8_t slot_index)
{
    if ((prefetch == NULL) || (prefetch->free_slots == NULL) ||
        (slot_index >= AUDIO_WAV_PREFETCH_SLOT_COUNT))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return (xQueueSend(prefetch->free_slots, &slot_index, 0U) == pdTRUE)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

const uint8_t *audio_wav_prefetch_slot_data(
    const audio_wav_prefetch_t *prefetch,
    uint8_t slot_index)
{
    if ((prefetch == NULL) || (slot_index >= AUDIO_WAV_PREFETCH_SLOT_COUNT))
    {
        return NULL;
    }

    return prefetch->slots[slot_index].data;
}

esp_err_t audio_wav_prefetch_get_info(
    const audio_wav_prefetch_t *prefetch,
    audio_wav_info_t *info)
{
    if ((prefetch == NULL) || (info == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!prefetch->info_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    *info = prefetch->info;
    return ESP_OK;
}

void audio_wav_prefetch_request_stop(audio_wav_prefetch_t *prefetch)
{
    if (prefetch == NULL)
    {
        return;
    }

    portENTER_CRITICAL(&prefetch->lock);
    prefetch->stop_requested = true;
    portEXIT_CRITICAL(&prefetch->lock);
}

esp_err_t audio_wav_prefetch_wait_stopped(
    audio_wav_prefetch_t *prefetch,
    TickType_t timeout)
{
    if (prefetch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!prefetch->started)
    {
        return ESP_OK;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        prefetch->events,
        AUDIO_WAV_PREFETCH_WORKER_STOPPED_BIT,
        pdFALSE,
        pdTRUE,
        timeout);
    return ((bits & AUDIO_WAV_PREFETCH_WORKER_STOPPED_BIT) != 0U)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t audio_wav_prefetch_get_worker_result(
    const audio_wav_prefetch_t *prefetch)
{
    if (prefetch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool worker_stopped;
    esp_err_t result;
    portENTER_CRITICAL((portMUX_TYPE *)&prefetch->lock);
    worker_stopped = prefetch->worker_stopped;
    result = prefetch->worker_result;
    portEXIT_CRITICAL((portMUX_TYPE *)&prefetch->lock);

    return worker_stopped ? result : ESP_ERR_INVALID_STATE;
}

esp_err_t audio_wav_prefetch_get_metrics(
    const audio_wav_prefetch_t *prefetch,
    audio_wav_prefetch_metrics_t *metrics)
{
    if ((prefetch == NULL) || (metrics == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool worker_stopped;
    portENTER_CRITICAL((portMUX_TYPE *)&prefetch->lock);
    worker_stopped = prefetch->worker_stopped;
    if (worker_stopped)
    {
        *metrics = prefetch->metrics;
    }
    portEXIT_CRITICAL((portMUX_TYPE *)&prefetch->lock);

    if (!worker_stopped)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t audio_wav_prefetch_stop_and_destroy(
    audio_wav_prefetch_t *prefetch,
    TickType_t timeout)
{
    if (prefetch == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!audio_wav_prefetch_is_active(prefetch))
    {
        return ESP_OK;
    }

    if (xTaskGetCurrentTaskHandle() != prefetch->owner_task)
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_wav_prefetch_request_stop(prefetch);
    const esp_err_t wait_result = audio_wav_prefetch_wait_stopped(
        prefetch,
        timeout);
    if (wait_result != ESP_OK)
    {
        return wait_result;
    }

    if (!prefetch->destroy_ack_sent)
    {
        prefetch->destroy_ack_sent = true;
        xEventGroupSetBits(
            prefetch->events,
            AUDIO_WAV_PREFETCH_WORKER_DESTROY_ACK_BIT);
    }

    if (ulTaskNotifyTake(pdTRUE, timeout) == 0U)
    {
        return ESP_ERR_TIMEOUT;
    }

    audio_wav_prefetch_free_resources(prefetch);
    memset(prefetch, 0, sizeof(*prefetch));
    return ESP_OK;
}

bool audio_wav_prefetch_is_active(const audio_wav_prefetch_t *prefetch)
{
    return (prefetch != NULL) &&
           (prefetch->started || (prefetch->task_handle != NULL) ||
            (prefetch->free_slots != NULL) ||
            (prefetch->ready_items != NULL) || (prefetch->events != NULL));
}
