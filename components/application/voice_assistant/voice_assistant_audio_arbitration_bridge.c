#include "audio_manager.h"
#include "audio_manager_arbitration.h"
#include "audio_manager_capture_arbiter.h"
#include "audio_manager_playback_arbiter.h"
#include "voice_assistant_audio_arbitration_bridge.h"

#include <stdatomic.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define XIAOZHI_ARB_WAIT_POLL_MS 20U
#define XIAOZHI_ARB_START_TIMEOUT_MS 3000U
#define XIAOZHI_CAPTURE_REQUEST_BASE 0x160C0000U
#define XIAOZHI_PLAYBACK_REQUEST_BASE 0x160E0000U

static const char *const TAG = "VOICE_AUDIO_ARB";
static atomic_uint_fast32_t s_capture_seq = 1U;
static atomic_uint_fast32_t s_playback_seq = 1U;
static atomic_uint_fast32_t s_capture_request_id = 0U;
static atomic_uint_fast32_t s_playback_request_id = 0U;

static uint32_t next_request_id(atomic_uint_fast32_t *sequence, uint32_t base)
{
    uint32_t value = (uint32_t)atomic_fetch_add_explicit(
        sequence, 1U, memory_order_relaxed);
    value &= 0x0000FFFFU;
    if (value == 0U) {
        value = 1U;
    }
    return base | value;
}

esp_err_t phase16_xiaozhi_start_recording(void)
{
    const uint32_t request_id = next_request_id(
        &s_capture_seq, XIAOZHI_CAPTURE_REQUEST_BASE);

    audio_manager_request_t request = {0};
    esp_err_t ret = audio_manager_request_make_default(
        request_id,
        AUDIO_MANAGER_CLIENT_XIAOZHI,
        AUDIO_MANAGER_RESOURCE_CAPTURE,
        &request);
    if (ret != ESP_OK) {
        return ret;
    }

    /* PTT capture should not wait behind another capture client. */
    request.busy_policy = AUDIO_MANAGER_BUSY_REJECT;
    request.priority = AUDIO_MANAGER_PRIORITY_XIAOZHI;
    request.interruptible = true;

    ret = audio_manager_capture_arbiter_submit(&request);
    if (ret != ESP_OK) {
        return ret;
    }
    atomic_store_explicit(&s_capture_request_id, request_id, memory_order_release);

    uint32_t waited = 0U;
    while (waited <= XIAOZHI_ARB_START_TIMEOUT_MS) {
        audio_manager_capture_arbiter_status_t status = {0};
        ret = audio_manager_capture_arbiter_get_status(&status);
        if (ret != ESP_OK) {
            break;
        }
        if (status.current_valid &&
            status.current.request_id == request_id &&
            status.state == AUDIO_MANAGER_CAPTURE_ARBITER_ACTIVE) {
            ESP_LOGI(TAG, "Xiaozhi capture granted request=%u", (unsigned)request_id);
            return ESP_OK;
        }
        if ((!status.current_valid || status.current.request_id != request_id) &&
            (!status.pending_valid || status.pending.request_id != request_id)) {
            ret = (status.last_error == ESP_OK) ? ESP_FAIL : status.last_error;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(XIAOZHI_ARB_WAIT_POLL_MS));
        waited += XIAOZHI_ARB_WAIT_POLL_MS;
    }

    (void)audio_manager_capture_arbiter_cancel(request_id);
    atomic_store_explicit(&s_capture_request_id, 0U, memory_order_release);
    return (ret == ESP_OK) ? ESP_ERR_TIMEOUT : ret;
}

esp_err_t phase16_xiaozhi_stop_recording(void)
{
    const uint32_t request_id = (uint32_t)atomic_exchange_explicit(
        &s_capture_request_id, 0U, memory_order_acq_rel);
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = audio_manager_capture_arbiter_cancel(request_id);
    return (ret == ESP_ERR_NOT_FOUND) ? ESP_ERR_INVALID_STATE : ret;
}

esp_err_t phase16_xiaozhi_stream_begin(void)
{
    if (atomic_load_explicit(&s_playback_request_id,
                             memory_order_acquire) != 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t request_id = next_request_id(
        &s_playback_seq, XIAOZHI_PLAYBACK_REQUEST_BASE);

    audio_manager_request_t request = {0};
    esp_err_t ret = audio_manager_request_make_default(
        request_id,
        AUDIO_MANAGER_CLIENT_XIAOZHI,
        AUDIO_MANAGER_RESOURCE_PLAYBACK,
        &request);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Xiaozhi response can be queued by policy clients, but remains
     * interruptible by a strictly-higher-priority critical alarm. */
    request.busy_policy = AUDIO_MANAGER_BUSY_QUEUE;
    request.priority = AUDIO_MANAGER_PRIORITY_XIAOZHI;
    request.interruptible = true;

    ret = audio_manager_playback_arbiter_submit_pcm16_stream(&request);
    if (ret != ESP_OK) {
        return ret;
    }
    atomic_store_explicit(&s_playback_request_id, request_id, memory_order_release);
    /* Do not wait for ACTIVE here: the manager starts I2S only after this
     * downlink worker has supplied a bounded PCM prefill. */
    ESP_LOGI(TAG, "Xiaozhi PCM stream reserved request=%u", (unsigned)request_id);
    return ESP_OK;
}

esp_err_t phase16_xiaozhi_stream_write(
    const int16_t *samples,
    size_t sample_count)
{
    const uint32_t request_id = (uint32_t)atomic_load_explicit(
        &s_playback_request_id,
        memory_order_acquire);
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    return audio_manager_playback_arbiter_write_pcm16(
        request_id,
        samples,
        sample_count);
}

esp_err_t phase16_xiaozhi_stream_finish(void)
{
    const uint32_t request_id = (uint32_t)atomic_load_explicit(
        &s_playback_request_id,
        memory_order_acquire);
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    return audio_manager_playback_arbiter_finish_pcm16_stream(request_id);
}

esp_err_t phase16_xiaozhi_stream_fail(esp_err_t error)
{
    if (error == ESP_OK) {
        error = ESP_FAIL;
    }
    const uint32_t request_id = (uint32_t)atomic_exchange_explicit(
        &s_playback_request_id,
        0U,
        memory_order_acq_rel);
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = audio_manager_playback_arbiter_fail_pcm16_stream(
        request_id,
        error);
    return (ret == ESP_ERR_NOT_FOUND) ? ESP_OK : ret;
}

esp_err_t phase16_xiaozhi_stream_get_status(
    audio_manager_playback_request_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint32_t request_id = (uint32_t)atomic_load_explicit(
        &s_playback_request_id,
        memory_order_acquire);
    if (request_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    return audio_manager_playback_arbiter_get_request_status(request_id, status);
}

void phase16_xiaozhi_stream_release(void)
{
    (void)atomic_exchange_explicit(
        &s_playback_request_id,
        0U,
        memory_order_acq_rel);
}
