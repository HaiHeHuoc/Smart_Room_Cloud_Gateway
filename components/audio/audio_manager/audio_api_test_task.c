#include "audio_api_test_task.h"

#include <stdbool.h>
#include <stdint.h>

#include "audio_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sd_card_manager.h"

#if CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST

/* --------------------------------------------------------------------------
 * Public-API hardware stress configuration
 * --------------------------------------------------------------------------
 * CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST is the production Kconfig gate. Edit
 * only this block when selecting an enabled hardware stress scenario.
 * The coordinator never touches I2S, DSP buffers, WAV internals, or SD FILE
 * objects directly; every audio operation goes through audio_manager public
 * APIs and public status snapshots. If SD VFS is unavailable, the WAV step is
 * skipped for that cycle so SD-independent record/playback stress continues.
 */

/*
 * Enable the operation groups independently.
 * FIXED:  audio_manager_record() -> audio_manager_play_recorded()
 * MANUAL: start_recording() -> hold -> stop_recording() -> play_recorded()
 * WAV:    audio_manager_play_wav(AUDIO_API_TEST_WAV_PATH)
 */
#define AUDIO_API_TEST_ENABLE_FIXED_RECORD             1U
#define AUDIO_API_TEST_ENABLE_MANUAL_RECORD            1U
#define AUDIO_API_TEST_ENABLE_WAV_PLAYBACK             1U

/* FreeRTOS coordinator settings. Priority 6 is the normal stress default.
 * Priority 7 may be tried only when intentionally measuring higher-priority
 * application pressure; the task still sleeps/polls between API operations.
 */
#define AUDIO_API_TEST_TASK_NAME                       "audio_api_test"
#define AUDIO_API_TEST_TASK_STACK_SIZE                 4096U
#define AUDIO_API_TEST_TASK_PRIORITY                   6U
#define AUDIO_API_TEST_POLL_MS                         100U

/* How long the public manual-record test holds before requesting stop. */
#define AUDIO_API_TEST_MANUAL_HOLD_MS                  3000U

/* 0 = start the next cycle immediately; non-zero adds a rest between cycles. */
#define AUDIO_API_TEST_INTER_CYCLE_DELAY_MS            1000U

/* Canonical PCM16 mono 16-kHz WAV below the mounted /sdcard filesystem. */
#define AUDIO_API_TEST_WAV_PATH "/sdcard/audio/input_long.wav"

#if !AUDIO_API_TEST_ENABLE_FIXED_RECORD && \
    !AUDIO_API_TEST_ENABLE_MANUAL_RECORD && \
    !AUDIO_API_TEST_ENABLE_WAV_PLAYBACK
#error "Enable at least one public audio stress operation"
#endif

static const char *const TAG = "AUDIO_API_TEST";
static TaskHandle_t s_test_task_handle = NULL;

/*
 * Hardware stress waits are intentionally unbounded. The task advances only
 * after public audio_manager status counters report a real terminal result.
 */
static void audio_api_test_wait_manager_idle(void)
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

static esp_err_t audio_api_test_wait_record_terminal(
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

static esp_err_t audio_api_test_wait_recorded_playback_terminal(
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

static esp_err_t audio_api_test_wait_wav_terminal(
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

static esp_err_t audio_api_test_fixed_record(void)
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

    result = audio_api_test_wait_record_terminal(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    before = (audio_manager_status_t) {0};
    result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    result = audio_manager_play_recorded();
    if (result != ESP_OK)
    {
        return result;
    }

    return audio_api_test_wait_recorded_playback_terminal(&before);
}

static esp_err_t audio_api_test_manual_record(void)
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

    result = audio_api_test_wait_record_terminal(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    before = (audio_manager_status_t) {0};
    result = audio_manager_get_status(&before);
    if (result != ESP_OK)
    {
        return result;
    }

    result = audio_manager_play_recorded();
    if (result != ESP_OK)
    {
        return result;
    }

    return audio_api_test_wait_recorded_playback_terminal(&before);
}

static esp_err_t audio_api_test_play_wav(void)
{
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

    return audio_api_test_wait_wav_terminal(&before);
}

static esp_err_t audio_api_test_run_cycle(
    uint32_t cycle,
    bool *wav_skipped)
{
    if (wav_skipped == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *wav_skipped = false;

    ESP_LOGI(
        TAG,
        "========== PUBLIC AUDIO STRESS CYCLE %u =========",
        (unsigned)cycle);

    esp_err_t result = ESP_OK;

    if (AUDIO_API_TEST_ENABLE_FIXED_RECORD != 0U)
    {
        ESP_LOGI(
            TAG,
            "CYCLE %u FIXED record -> recorded playback",
            (unsigned)cycle);
        result = audio_api_test_fixed_record();
        if (result != ESP_OK)
        {
            return result;
        }
    }

    if (AUDIO_API_TEST_ENABLE_MANUAL_RECORD != 0U)
    {
        ESP_LOGI(
            TAG,
            "CYCLE %u MANUAL record hold=%ums -> recorded playback",
            (unsigned)cycle,
            (unsigned)AUDIO_API_TEST_MANUAL_HOLD_MS);
        result = audio_api_test_manual_record();
        if (result != ESP_OK)
        {
            return result;
        }
    }

    if (AUDIO_API_TEST_ENABLE_WAV_PLAYBACK != 0U)
    {
        if (!sd_card_manager_is_mounted())
        {
            *wav_skipped = true;
            ESP_LOGW(
                TAG,
                "CYCLE %u WAV skipped: SD VFS is not READY; "
                "continuing record/playback stress",
                (unsigned)cycle);
        }
        else
        {
            ESP_LOGI(
                TAG,
                "CYCLE %u WAV playback: %s",
                (unsigned)cycle,
                AUDIO_API_TEST_WAV_PATH);
            result = audio_api_test_play_wav();
            if (result != ESP_OK)
            {
                return result;
            }
        }
    }

    return ESP_OK;
}

static void audio_api_test_task(void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Public audio API stress started: priority=%u fixed=%u manual=%u "
        "wav=%u sd_unavailable=skip continuous=yes",
        (unsigned)AUDIO_API_TEST_TASK_PRIORITY,
        (unsigned)AUDIO_API_TEST_ENABLE_FIXED_RECORD,
        (unsigned)AUDIO_API_TEST_ENABLE_MANUAL_RECORD,
        (unsigned)AUDIO_API_TEST_ENABLE_WAV_PLAYBACK);

    audio_api_test_wait_manager_idle();

    uint32_t cycle = 1U;
    uint32_t completed = 0U;
    uint32_t failed = 0U;
    uint32_t wav_skipped = 0U;

    while (true)
    {
        bool cycle_wav_skipped = false;
        const esp_err_t result =
            audio_api_test_run_cycle(
                cycle,
                &cycle_wav_skipped);
        if (result == ESP_OK)
        {
            ++completed;
            if (cycle_wav_skipped)
            {
                ++wav_skipped;
                ESP_LOGW(
                    TAG,
                    "PUBLIC AUDIO STRESS CYCLE %u PARTIAL: "
                    "record/playback passed, WAV skipped; "
                    "totals: completed=%u wav_skip=%u fail=%u",
                    (unsigned)cycle,
                    (unsigned)completed,
                    (unsigned)wav_skipped,
                    (unsigned)failed);
            }
            else
            {
                ESP_LOGI(
                    TAG,
                    "PUBLIC AUDIO STRESS CYCLE %u PASS totals: "
                    "completed=%u wav_skip=%u fail=%u",
                    (unsigned)cycle,
                    (unsigned)completed,
                    (unsigned)wav_skipped,
                    (unsigned)failed);
            }
        }
        else
        {
            ++failed;
            audio_manager_status_t status = {0};
            (void)audio_manager_get_status(&status);
            ESP_LOGE(
                TAG,
                "PUBLIC AUDIO STRESS CYCLE %u FAIL: %s state=%s "
                "last_error=%s totals: completed=%u wav_skip=%u fail=%u",
                (unsigned)cycle,
                esp_err_to_name(result),
                audio_manager_state_to_string(status.state),
                esp_err_to_name(status.last_error),
                (unsigned)completed,
                (unsigned)wav_skipped,
                (unsigned)failed);
        }

        /*
         * A failed operation must clean itself back to IDLE. Waiting forever
         * here intentionally exposes a stuck production lifecycle instead of
         * hiding it with a private test-only reset path.
         */
        audio_api_test_wait_manager_idle();

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
        audio_api_test_task,
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

#else

esp_err_t app_audio_api_test_task_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST */
