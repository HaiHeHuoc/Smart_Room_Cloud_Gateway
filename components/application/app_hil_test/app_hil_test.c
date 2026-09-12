#include "app_hil_test.h"

#include "sdkconfig.h"

#if CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST
#include "audio_api_test_task.h"
#include "app_log.h"
#endif

#if CONFIG_APP_PHASE16_AUTO_HIL_TEST
#include "app_hil_phase16_auto.h"
#include "app_log.h"
#endif

#if CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST || CONFIG_APP_PHASE16_AUTO_HIL_TEST
static const char *const TAG = "APP_HIL_TEST";
#endif

esp_err_t app_hil_test_start_after_audio_ready(void)
{
#if CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST
    const esp_err_t audio_test_ret = app_audio_api_test_task_start();
    if (audio_test_ret != ESP_OK) {
        APP_LOGW(TAG, PUBLIC_AUDIO_TEST_START_FAILED_9760D176,
                 "Failed to start public audio API validation task: %s",
                 esp_err_to_name(audio_test_ret));
    } else {
        APP_LOGI(TAG, PUBLIC_AUDIO_TEST_STARTED_22F9B09B,
                 "Public audio API validation task started at priority 6");
    }
#endif

#if CONFIG_APP_PHASE16_AUTO_HIL_TEST
    const esp_err_t phase16_test_ret = app_hil_phase16_auto_start();
    if (phase16_test_ret != ESP_OK) {
        APP_LOGW(TAG, PHASE16_AUTO_HIL_START_FAILED_97D8693F,
                 "Failed to start Phase-16 automatic HIL coordinator: %s",
                 esp_err_to_name(phase16_test_ret));
        return phase16_test_ret;
    }

    APP_LOGI(TAG, PHASE16_AUTO_HIL_STARTED_20DF55AD,
             "Phase-16 automatic HIL coordinator started (test profile only)");
#endif

    return ESP_OK;
}
