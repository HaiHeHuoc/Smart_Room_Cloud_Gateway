#include "audio_manager.h"
#include "audio_manager_stream.h"
#include "board_config.h"
#include "esp_log.h"
#include "voice_assistant.h"
#include "voice_assistant_audio_adapter.h"
#include "voice_assistant_downlink.h"
#include "voice_assistant_ptt.h"
#include "voice_assistant_ptt_gpio.h"
#include "voice_assistant_uplink.h"

static const char *const TAG = "PH14_COMPOSE";

static audio_manager_status_callback_t s_app_audio_callback = NULL;
static void *s_app_audio_callback_context = NULL;
static bool s_voice_started = false;

static void phase14_audio_status_fanout(
    const audio_manager_status_t *status,
    void *user_context)
{
    (void)user_context;

    if (status == NULL) {
        return;
    }

    if (s_app_audio_callback != NULL) {
        s_app_audio_callback(status, s_app_audio_callback_context);
    }

    const esp_err_t voice_ret =
        voice_assistant_audio_adapter_post(status);
    if ((voice_ret != ESP_OK) &&
        (voice_ret != ESP_ERR_INVALID_STATE) &&
        (voice_ret != ESP_ERR_TIMEOUT)) {
        ESP_LOGD(TAG,
                 "voice audio-status fanout dropped: %s",
                 esp_err_to_name(voice_ret));
    }
}

esp_err_t app_phase14_audio_manager_register_status_callback(
    audio_manager_status_callback_t callback,
    void *user_context)
{
    s_app_audio_callback = callback;
    s_app_audio_callback_context = user_context;

    return audio_manager_register_status_callback(
        phase14_audio_status_fanout,
        NULL);
}

static esp_err_t phase14_start_voice_stack(void)
{
    if (s_voice_started) {
        return ESP_OK;
    }

    esp_err_t ret = voice_assistant_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_start();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_ptt_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_ptt_start();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_uplink_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_uplink_start();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_downlink_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_downlink_start();
    if (ret != ESP_OK) {
        return ret;
    }

    const voice_assistant_ptt_gpio_config_t ptt_gpio_config = {
        .gpio_num = PTT_BUTTON_GPIO,
        .active_level = PTT_BUTTON_ACTIVE_LEVEL,
        .poll_period_ms = PTT_BUTTON_POLL_PERIOD_MS,
        .debounce_ms = PTT_BUTTON_DEBOUNCE_MS,
    };

    ret = voice_assistant_ptt_gpio_init(&ptt_gpio_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_ptt_gpio_start();
    if (ret != ESP_OK) {
        return ret;
    }

    s_voice_started = true;
    ESP_LOGI(TAG,
             "Phase-14 voice stack READY ptt_gpio=%d active_level=%u pull=down",
             (int)PTT_BUTTON_GPIO,
             (unsigned)PTT_BUTTON_ACTIVE_LEVEL);
    return ESP_OK;
}

esp_err_t app_phase14_audio_manager_start(void)
{
    const esp_err_t audio_ret = audio_manager_start();
    if (audio_ret != ESP_OK) {
        return audio_ret;
    }

    const esp_err_t voice_ret = phase14_start_voice_stack();
    if (voice_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Phase-14 voice stack startup failed after audio READY: %s",
                 esp_err_to_name(voice_ret));
        return voice_ret;
    }

    return ESP_OK;
}
