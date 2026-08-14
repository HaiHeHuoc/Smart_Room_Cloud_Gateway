#include "audio_api_test_task.h"

#include <stdint.h>

#include "audio_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card_manager.h"

/* Test policy -------------------------------------------------------------- */
#define AUDIO_API_TEST_TASK_NAME                 "audio_api_test"
#define AUDIO_API_TEST_TASK_STACK_SIZE           4096U
#define AUDIO_API_TEST_TASK_PRIORITY             6U
#define AUDIO_API_TEST_POLL_MS                   100U
#define AUDIO_API_TEST_MANUAL_HOLD_MS            3000U
#define AUDIO_API_TEST_WAV_PATH "/sdcard/audio/input_2.wav"

static const char *const TAG = "AUDIO_API_TEST";
static TaskHandle_t s_test_task_handle = NULL;

/*
 * This is a hardware validation/soak task, not a bounded unit test. Waiting
 * helpers intentionally have no timeout: an operation is allowed to run for
 * its real duration and the task advances only after the public status
 * counters report a real terminal result.
 */
static esp_err_t app_audio_api_test_wait_manager_idle(void)
{
    while (true)
    {
        audio_manager_status_t status = {0};
        const esp_err_t result = audio_manager_get_status(&status);
        if ((result == ESP_OK) &&
            (status.state == AUDIO_MANAGER_STATE_IDLE))
        {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
    }
}

static esp_err_t app_audio_api_test_wait_record_terminal(
    const audio_manager_status_t *before)
{
    if (before == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    while (true)
    {
        audio_manager_status_t status = {0};
        const esp_err_t result = audio_manager_get_status(&status);
        if (result != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
            continue;
        }

        if (status.recording_failed > before->recording_failed)
        {
            return (status.last_error != ESP_OK)
                       ? status.last_error
                       : ESP_FAIL;
        }

        if (status.recording_completed > before->recording_completed)
        {
            return (status.recorded_audio_available &&
                    (status.state == AUDIO_MANAGER_STATE_IDLE))
                       ? ESP_OK
                       : ESP_ERR_INVALID_STATE;
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
    }
}

static esp_err_t app_audio_api_test_wait_recorded_playback_terminal(
    const audio_manager_status_t *before)
{
    if (before == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    while (true)
    {
        audio_manager_status_t status = {0};
        const esp_err_t result = audio_manager_get_status(&status);
        if (result != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
            continue;
        }

        if (status.recorded_playback_failed > before->recorded_playback_failed)
        {
            return (status.last_error != ESP_OK)
                       ? status.last_error
                       : ESP_FAIL;
        }

        if (status.recorded_playback_cancelled >
            before->recorded_playback_cancelled)
        {
            return ESP_ERR_INVALID_STATE;
        }

        if (status.recorded_playback_completed >
            before->recorded_playback_completed)
        {
            return (status.state == AUDIO_MANAGER_STATE_IDLE)
                       ? ESP_OK
                       : ESP_ERR_INVALID_STATE;
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
    }
}

static esp_err_t app_audio_api_test_wait_wav_terminal(
    const audio_manager_status_t *before)
{
    if (before == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    while (true)
    {
        audio_manager_status_t status = {0};
        const esp_err_t result = audio_manager_get_status(&status);
        if (result != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
            continue;
        }

        if (status.wav_playback_failed > before->wav_playback_failed)
        {
            return (status.last_error != ESP_OK)
                       ? status.last_error
                       : ESP_FAIL;
        }

        if (status.wav_playback_cancelled > before->wav_playback_cancelled)
        {
            return ESP_ERR_INVALID_STATE;
        }

        if (status.wav_playback_completed > before->wav_playback_completed)
        {
            return (status.state == AUDIO_MANAGER_STATE_IDLE)
                       ? ESP_OK
                       : ESP_ERR_INVALID_STATE;
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
    }
}

static void app_audio_api_test_wait_sd_ready(void)
{
    bool waiting_logged = false;

    while (!sd_card_manager_is_mounted())
    {
        if (!waiting_logged)
        {
            ESP_LOGI(TAG, "Waiting indefinitely for SD READY before WAV test");
            waiting_logged = true;
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
    }

    if (waiting_logged)
    {
        ESP_LOGI(TAG, "SD READY; continuing WAV test");
    }
}

static esp_err_t app_audio_api_test_fixed_record(void)
{
    audio_manager_status_t before = {0};
    esp_err_t result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(TAG, "STEP 1/5 fixed record: request");
    result = audio_manager_record();
    if (result != ESP_OK)
    {
        return result;
    }

    return app_audio_api_test_wait_record_terminal(&before);
}

static esp_err_t app_audio_api_test_play_recorded(const char *step_name)
{
    audio_manager_status_t before = {0};
    esp_err_t result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(TAG, "%s", step_name);
    result = audio_manager_play_recorded();
    if (result != ESP_OK)
    {
        return result;
    }

    return app_audio_api_test_wait_recorded_playback_terminal(&before);
}

static esp_err_t app_audio_api_test_manual_record(void)
{
    audio_manager_status_t before = {0};
    esp_err_t result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(
        TAG,
        "STEP 3/5 manual record: start, hold %ums, stop",
        (unsigned)AUDIO_API_TEST_MANUAL_HOLD_MS);

    result = audio_manager_start_recording();
    if (result != ESP_OK)
    {
        return result;
    }

    vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_MANUAL_HOLD_MS));

    const esp_err_t stop_result = audio_manager_stop_recording();
    if ((stop_result != ESP_OK) &&
        (stop_result != ESP_ERR_INVALID_STATE))
    {
        return stop_result;
    }

    if (stop_result == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(
            TAG,
            "Manual stop found no active recording; operation may already have reached its configured maximum");
    }

    return app_audio_api_test_wait_record_terminal(&before);
}

static esp_err_t app_audio_api_test_play_wav(void)
{
    ESP_LOGI(TAG, "STEP 5/5 wait for SD before WAV playback");
    app_audio_api_test_wait_sd_ready();

    audio_manager_status_t before = {0};
    esp_err_t result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(TAG, "STEP 5/5 WAV playback: %s", AUDIO_API_TEST_WAV_PATH);
    result = audio_manager_play_wav(AUDIO_API_TEST_WAV_PATH);
    if (result != ESP_OK)
    {
        return result;
    }

    return app_audio_api_test_wait_wav_terminal(&before);
}

static void app_audio_api_test_task(void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Public audio API validation started: priority=%u timeout=disabled",
        (unsigned)AUDIO_API_TEST_TASK_PRIORITY);

    esp_err_t result = app_audio_api_test_wait_manager_idle();

    if (result == ESP_OK)
    {
        result = app_audio_api_test_fixed_record();
    }
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "STEP 1/5 PASS fixed record");
        result = app_audio_api_test_play_recorded(
            "STEP 2/5 play retained fixed recording");
    }
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "STEP 2/5 PASS recorded playback");
        result = app_audio_api_test_manual_record();
    }
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "STEP 3/5 PASS manual record");
        result = app_audio_api_test_play_recorded(
            "STEP 4/5 play retained manual recording");
    }
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "STEP 4/5 PASS recorded playback");
        result = app_audio_api_test_play_wav();
    }
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "STEP 5/5 PASS WAV playback");
        ESP_LOGI(TAG, "PUBLIC AUDIO API TEST PASS");
    }
    else
    {
        audio_manager_status_t status = {0};
        (void)audio_manager_get_status(&status);
        ESP_LOGE(
            TAG,
            "PUBLIC AUDIO API TEST FAIL: %s state=%s last_error=%s",
            esp_err_to_name(result),
            audio_manager_state_to_string(status.state),
            esp_err_to_name(status.last_error));
    }

    s_test_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_audio_api_test_task_start(void)
{
    if (s_test_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t result = xTaskCreate(
        app_audio_api_test_task,
        AUDIO_API_TEST_TASK_NAME,
        AUDIO_API_TEST_TASK_STACK_SIZE,
        NULL,
        AUDIO_API_TEST_TASK_PRIORITY,
        &s_test_task_handle);

    if (result != pdPASS)
    {
        s_test_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
