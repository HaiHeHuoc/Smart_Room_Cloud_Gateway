#include "voice_assistant_downlink.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "audio_manager.h"
#include "sd_card_manager.h"
#include "xiaozhi_foundation.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DOWNLINK_TASK_NAME                 "voice_downlink"
#define DOWNLINK_TASK_STACK_BYTES          5120U
#define DOWNLINK_TASK_PRIORITY             5U
#define DOWNLINK_QUEUE_LENGTH              8U
#define DOWNLINK_CHUNK_BYTES               2048U
#define DOWNLINK_RESPONSE_CAPACITY_BYTES   (1024U * 1024U)
#define DOWNLINK_AUDIO_IDLE_TIMEOUT_MS      10000U
#define DOWNLINK_AUDIO_IDLE_POLL_MS         50U
#define DOWNLINK_WAV_PATH                  "/sdcard/xiaozhi_response.wav"

_Static_assert((DOWNLINK_RESPONSE_CAPACITY_BYTES % 2U) == 0U,
               "PCM16 response capacity must be sample aligned");

typedef struct {
    xiaozhi_foundation_response_event_kind_t kind;
    uint32_t generation;
    size_t data_len;
    esp_err_t error;
    uint8_t data[DOWNLINK_CHUNK_BYTES];
} downlink_item_t;

static const char *const TAG = "VOICE_DOWNLINK";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_queue = NULL;
static TaskHandle_t s_task = NULL;
static uint8_t *s_response = NULL;
static size_t s_response_size = 0U;
static voice_assistant_downlink_status_t s_status = {0};

static void downlink_set_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.last_error = (error == ESP_OK) ? ESP_FAIL : error;
    ++s_status.responses_failed;
    portEXIT_CRITICAL(&s_lock);
}

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8U) & 0xffU);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8U) & 0xffU);
    dst[2] = (uint8_t)((value >> 16U) & 0xffU);
    dst[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static esp_err_t downlink_write_wav(const uint8_t *pcm, size_t pcm_bytes)
{
    if ((pcm == NULL) || (pcm_bytes == 0U) || ((pcm_bytes % 2U) != 0U) ||
        (pcm_bytes > UINT32_MAX - 36U)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = sd_card_manager_acquire();
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *file = fopen(DOWNLINK_WAV_PATH, "wb");
    if (file == NULL) {
        const int saved_errno = errno;
        sd_card_manager_release();
        if (sd_card_manager_is_vfs_media_error(saved_errno)) {
            sd_card_manager_report_io_error(ESP_FAIL);
        }
        return ESP_FAIL;
    }

    uint8_t header[44] = {0};
    memcpy(&header[0], "RIFF", 4U);
    put_u32_le(&header[4], (uint32_t)(36U + pcm_bytes));
    memcpy(&header[8], "WAVEfmt ", 8U);
    put_u32_le(&header[16], 16U);
    put_u16_le(&header[20], 1U);
    put_u16_le(&header[22], 1U);
    put_u32_le(&header[24], XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ);
    put_u32_le(&header[28], XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ * 2U);
    put_u16_le(&header[32], 2U);
    put_u16_le(&header[34], 16U);
    memcpy(&header[36], "data", 4U);
    put_u32_le(&header[40], (uint32_t)pcm_bytes);

    bool io_failed = false;
    if (fwrite(header, 1U, sizeof(header), file) != sizeof(header)) {
        io_failed = true;
    }
    if (!io_failed && (fwrite(pcm, 1U, pcm_bytes, file) != pcm_bytes)) {
        io_failed = true;
    }
    if (!io_failed && (fflush(file) != 0)) {
        io_failed = true;
    }
    if (fclose(file) != 0) {
        io_failed = true;
    }
    sd_card_manager_release();

    if (io_failed) {
        sd_card_manager_report_io_error(ESP_FAIL);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t downlink_wait_audio_idle(void)
{
    uint32_t waited = 0U;
    while (waited <= DOWNLINK_AUDIO_IDLE_TIMEOUT_MS) {
        audio_manager_status_t audio = {0};
        const esp_err_t ret = audio_manager_get_status(&audio);
        if (ret != ESP_OK) {
            return ret;
        }
        if (audio.state == AUDIO_MANAGER_STATE_IDLE) {
            return ESP_OK;
        }
        if ((audio.state == AUDIO_MANAGER_STATE_ERROR) ||
            (audio.state == AUDIO_MANAGER_STATE_UNINITIALIZED) ||
            (audio.state == AUDIO_MANAGER_STATE_INITIALIZED)) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(DOWNLINK_AUDIO_IDLE_POLL_MS));
        waited += DOWNLINK_AUDIO_IDLE_POLL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t downlink_finalize_response(uint32_t generation)
{
    if ((generation == 0U) || (s_response_size == 0U) ||
        ((s_response_size % sizeof(int16_t)) != 0U)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t first_error = xiaozhi_foundation_audio_channel_close(generation);
    if ((first_error != ESP_OK) && (first_error != ESP_ERR_INVALID_STATE)) {
        return first_error;
    }

    first_error = downlink_write_wav(s_response, s_response_size);
    if (first_error != ESP_OK) {
        return first_error;
    }

    first_error = downlink_wait_audio_idle();
    if (first_error != ESP_OK) {
        return first_error;
    }

    first_error = audio_manager_play_wav(DOWNLINK_WAV_PATH);
    if (first_error != ESP_OK) {
        return first_error;
    }

    portENTER_CRITICAL(&s_lock);
    s_status.playback_requested = true;
    ++s_status.responses_completed;
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG,
             "response PLAYBACK_REQUESTED generation=%u pcm_bytes=%u path=%s",
             (unsigned)generation,
             (unsigned)s_response_size,
             DOWNLINK_WAV_PATH);
    return ESP_OK;
}

static void downlink_response_callback(
    const xiaozhi_foundation_response_event_t *event,
    void *user_context)
{
    (void)user_context;
    if ((event == NULL) || (s_queue == NULL)) {
        return;
    }

    if (event->kind != XIAOZHI_FOUNDATION_RESPONSE_AUDIO) {
        const downlink_item_t item = {
            .kind = event->kind,
            .generation = event->client_generation,
            .error = event->error,
        };
        if (xQueueSend(s_queue, &item, 0U) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            ++s_status.chunks_dropped_queue_full;
            portEXIT_CRITICAL(&s_lock);
        }
        return;
    }

    if ((event->data == NULL) || (event->data_len == 0U)) {
        return;
    }

    size_t offset = 0U;
    while (offset < event->data_len) {
        size_t chunk = event->data_len - offset;
        if (chunk > DOWNLINK_CHUNK_BYTES) {
            chunk = DOWNLINK_CHUNK_BYTES;
        }
        downlink_item_t item = {
            .kind = XIAOZHI_FOUNDATION_RESPONSE_AUDIO,
            .generation = event->client_generation,
            .data_len = chunk,
            .error = ESP_OK,
        };
        memcpy(item.data, &event->data[offset], chunk);
        if (xQueueSend(s_queue, &item, 0U) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            ++s_status.chunks_dropped_queue_full;
            portEXIT_CRITICAL(&s_lock);
            break;
        }
        portENTER_CRITICAL(&s_lock);
        ++s_status.chunks_queued;
        s_status.response_bytes_received += chunk;
        portEXIT_CRITICAL(&s_lock);
        offset += chunk;
    }
}

static void downlink_task(void *argument)
{
    (void)argument;
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "coordinator started capacity=%uB",
             (unsigned)DOWNLINK_RESPONSE_CAPACITY_BYTES);

    for (;;) {
        downlink_item_t item = {0};
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_TTS_START) {
            s_response_size = 0U;
            portENTER_CRITICAL(&s_lock);
            s_status.collecting = true;
            s_status.playback_requested = false;
            s_status.session_generation = item.generation;
            s_status.response_bytes_buffered = 0U;
            s_status.last_error = ESP_OK;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "response START generation=%u", (unsigned)item.generation);
            continue;
        }

        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_AUDIO) {
            bool accept = false;
            portENTER_CRITICAL(&s_lock);
            accept = s_status.collecting &&
                     (s_status.session_generation == item.generation);
            portEXIT_CRITICAL(&s_lock);
            if (!accept) {
                continue;
            }
            if ((s_response_size + item.data_len) >
                DOWNLINK_RESPONSE_CAPACITY_BYTES) {
                downlink_set_error(ESP_ERR_NO_MEM);
                portENTER_CRITICAL(&s_lock);
                s_status.collecting = false;
                portEXIT_CRITICAL(&s_lock);
                (void)xiaozhi_foundation_audio_channel_close(item.generation);
                ESP_LOGE(TAG, "response buffer overflow generation=%u",
                         (unsigned)item.generation);
                continue;
            }
            memcpy(&s_response[s_response_size], item.data, item.data_len);
            s_response_size += item.data_len;
            portENTER_CRITICAL(&s_lock);
            s_status.response_bytes_buffered = s_response_size;
            portEXIT_CRITICAL(&s_lock);
            continue;
        }

        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_ERROR) {
            portENTER_CRITICAL(&s_lock);
            s_status.collecting = false;
            portEXIT_CRITICAL(&s_lock);
            (void)xiaozhi_foundation_audio_channel_close(item.generation);
            downlink_set_error((item.error == ESP_OK) ? ESP_FAIL : item.error);
            ESP_LOGE(TAG, "response ERROR generation=%u error=%s",
                     (unsigned)item.generation,
                     esp_err_to_name((item.error == ESP_OK) ? ESP_FAIL : item.error));
            continue;
        }

        if (item.kind == XIAOZHI_FOUNDATION_RESPONSE_TTS_STOP) {
            bool accept = false;
            portENTER_CRITICAL(&s_lock);
            accept = s_status.collecting &&
                     (s_status.session_generation == item.generation);
            s_status.collecting = false;
            portEXIT_CRITICAL(&s_lock);
            if (!accept) {
                continue;
            }
            const esp_err_t ret = downlink_finalize_response(item.generation);
            if (ret != ESP_OK) {
                downlink_set_error(ret);
                ESP_LOGE(TAG, "response finalize failed generation=%u error=%s",
                         (unsigned)item.generation,
                         esp_err_to_name(ret));
            }
        }
    }
}

esp_err_t voice_assistant_downlink_init(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_status.initialized) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_lock);

    s_queue = xQueueCreate(DOWNLINK_QUEUE_LENGTH, sizeof(downlink_item_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_response = (uint8_t *)heap_caps_malloc(
        DOWNLINK_RESPONSE_CAPACITY_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_response == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_response_size = 0U;
    portENTER_CRITICAL(&s_lock);
    s_status = (voice_assistant_downlink_status_t) {
        .initialized = true,
        .last_error = ESP_OK,
    };
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t voice_assistant_downlink_start(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool initialized = s_status.initialized;
    const bool running = s_status.running || (s_task != NULL);
    portEXIT_CRITICAL(&s_lock);
    if (!initialized || (s_queue == NULL) || (s_response == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (running) {
        return ESP_OK;
    }

    esp_err_t ret = xiaozhi_foundation_response_register_callback(
        downlink_response_callback, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xTaskCreate(
            downlink_task,
            DOWNLINK_TASK_NAME,
            DOWNLINK_TASK_STACK_BYTES,
            NULL,
            DOWNLINK_TASK_PRIORITY,
            &s_task) != pdPASS) {
        (void)xiaozhi_foundation_response_register_callback(NULL, NULL);
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t voice_assistant_downlink_get_status(
    voice_assistant_downlink_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
