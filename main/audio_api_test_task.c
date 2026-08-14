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
#define AUDIO_API_TEST_INTER_CYCLE_DELAY_MS      1000U
#define AUDIO_API_TEST_WAV_PATH "/sdcard/audio/input_2.wav"

static const char *const TAG = "AUDIO_API_TEST";
static TaskHandle_t s_test_task_handle = NULL;

/*
 * Hardware stress waits are intentionally unbounded. The task advances only
 * after public audio_manager status counters report a real terminal result.
 */
static void app_audio_api_test_wait_manager_idle(void)
{
    while (true)
    {
        audio_manager_status_t status = {0};
        if ((audio_manager_get_status(&status) == ESP_OK) &&
            (status.state == AUDIO_MANAGER_STATE_IDLE))
        {
            return;
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
            ESP_LOGI(TAG, "Waiting indefinitely for SD READY before WAV stress step");
            waiting_logged = true;
        }

        vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_POLL_MS));
    }

    if (waiting_logged)
    {
        ESP_LOGI(TAG, "SD READY; continuing WAV stress step");
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

    result = audio_manager_record();
    if (result != ESP_OK)
    {
        return result;
    }

    return app_audio_api_test_wait_record_terminal(&before);
}

static esp_err_t app_audio_api_test_play_recorded(void)
{
    audio_manager_status_t before = {0};
    esp_err_t result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

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
            "Manual stop found no active recording; configured maximum may already be complete");
    }

    return app_audio_api_test_wait_record_terminal(&before);
}

static esp_err_t app_audio_api_test_play_wav(void)
{
    app_audio_api_test_wait_sd_ready();

    audio_manager_status_t before = {0};
    esp_err_t result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    result = audio_manager_play_wav(AUDIO_API_TEST_WAV_PATH);
    if (result != ESP_OK)
    {
        return result;
    }

    return app_audio_api_test_wait_wav_terminal(&before);
}

static esp_err_t app_audio_api_test_run_cycle(uint32_t cycle)
{
    ESP_LOGI(TAG, "========== PUBLIC AUDIO STRESS CYCLE %u =========", (unsigned)cycle);

    ESP_LOGI(TAG, "CYCLE %u STEP 1/5 fixed record", (unsigned)cycle);
    esp_err_t result = app_audio_api_test_fixed_record();
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(TAG, "CYCLE %u STEP 2/5 play fixed recording", (unsigned)cycle);
    result = app_audio_api_test_play_recorded();
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(
        TAG,
        "CYCLE %u STEP 3/5 manual record hold=%ums",
        (unsigned)cycle,
        (unsigned)AUDIO_API_TEST_MANUAL_HOLD_MS);
    result = app_audio_api_test_manual_record();
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(TAG, "CYCLE %u STEP 4/5 play manual recording", (unsigned)cycle);
    result = app_audio_api_test_play_recorded();
    if (result != ESP_OK)
    {
        return result;
    }

    ESP_LOGI(
        TAG,
        "CYCLE %u STEP 5/5 WAV playback: %s",
        (unsigned)cycle,
        AUDIO_API_TEST_WAV_PATH);
    return app_audio_api_test_play_wav();
}

static void app_audio_api_test_task(void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Public audio API stress test started: priority=%u timeout=disabled continuous=yes",
        (unsigned)AUDIO_API_TEST_TASK_PRIORITY);

    app_audio_api_test_wait_manager_idle();

    uint32_t cycle = 1U;
    uint32_t passed = 0U;
    uint32_t failed = 0U;

    while (true)
    {
        const esp_err_t result = app_audio_api_test_run_cycle(cycle);
        if (result == ESP_OK)
        {
            ++passed;
            ESP_LOGI(
                TAG,
                "PUBLIC AUDIO STRESS CYCLE %u PASS totals: pass=%u fail=%u",
                (unsigned)cycle,
                (unsigned)passed,
                (unsigned)failed);
        }
        else
        {
            ++failed;
            audio_manager_status_t status = {0};
            (void)audio_manager_get_status(&status);
            ESP_LOGE(
                TAG,
                "PUBLIC AUDIO STRESS CYCLE %u FAIL: %s state=%s last_error=%s totals: pass=%u fail=%u",
                (unsigned)cycle,
                esp_err_to_name(result),
                audio_manager_state_to_string(status.state),
                esp_err_to_name(status.last_error),
                (unsigned)passed,
                (unsigned)failed);
        }

        /*
         * A failed operation is expected to clean itself back to IDLE. If it
         * does not, waiting here intentionally exposes the stuck lifecycle on
         * hardware instead of hiding it with task-local recovery shortcuts.
         */
        app_audio_api_test_wait_manager_idle();

        ++cycle;
        if (cycle == 0U)
        {
            cycle = 1U;
        }

        if (AUDIO_API_TEST_INTER_CYCLE_DELAY_MS > 0U)
        {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_API_TEST_INTER_CYCLE_DELAY_MS));
        }
    }
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
