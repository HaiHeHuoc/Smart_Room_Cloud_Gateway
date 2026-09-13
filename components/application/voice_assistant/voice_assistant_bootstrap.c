#include "voice_assistant.h"

#include "audio_manager_capture_arbiter.h"
#include "audio_manager_playback_arbiter.h"
#include "board_config.h"
#include "esp_log.h"
#include "app_log.h"
#include "voice_assistant_audio_adapter.h"
#include "voice_assistant_downlink.h"
#include "voice_assistant_ptt.h"
#include "voice_assistant_ptt_gpio.h"
#include "voice_assistant_ui_gui_adapter.h"
#include "voice_assistant_ui_model.h"
#include "voice_assistant_uplink.h"

_Static_assert(PTT_BUTTON_USE_INTERNAL_PULLDOWN == 1,
               "Voice PTT GPIO contract requires internal pull-down");

static const char *const TAG = "VOICE_BOOTSTRAP";

static audio_manager_status_callback_t s_app_audio_callback = NULL;
static void *s_app_audio_callback_context = NULL;
static bool s_voice_started = false;

static void voice_assistant_audio_status_fanout(
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
        APP_LOGD(TAG, VOICE_AUDIO_STATUS_FANOUT_DR_176810ED,
                 "voice audio-status fanout dropped: %s",
                 esp_err_to_name(voice_ret));
    }
}

static esp_err_t voice_assistant_start_stack(void)
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

    ret = voice_assistant_ui_model_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_ui_model_start();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_ui_gui_adapter_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = voice_assistant_ui_gui_adapter_start();
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

    /* Start the long-lived session as soon as the audio stack is ready. The
     * PTT path then only opens a turn; it must not become a boot-time
     * connection gate. */
    ret = voice_assistant_begin_session();
    if (ret != ESP_OK) {
        APP_LOGE(TAG, VOICE_BOOT_CONNECTION_FAILED_9A77647E,
                 "Boot Xiaozhi connection request failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    s_voice_started = true;
    APP_LOGI(TAG, VOICE_STACK_READY_AFTER_AUDIO_35809038,
             "Voice stack READY; boot Xiaozhi connection queued ui_model=yes gui_adapter=yes ptt_gpio=%d active_level=%u pull=down",
             (int)PTT_BUTTON_GPIO,
             (unsigned)PTT_BUTTON_ACTIVE_LEVEL);
    return ESP_OK;
}

esp_err_t voice_assistant_start_after_audio_ready(
    audio_manager_status_callback_t application_callback,
    void *application_callback_context)
{
    s_app_audio_callback = application_callback;
    s_app_audio_callback_context = application_callback_context;

    const esp_err_t observer_ret = audio_manager_register_status_callback(
        voice_assistant_audio_status_fanout,
        NULL);
    if (observer_ret != ESP_OK) {
        APP_LOGW(TAG, AUDIO_STATUS_OBSERVER_UNAVAILABLE_F6599647,
                 "Audio status observer unavailable: %s",
                 esp_err_to_name(observer_ret));
    }

    const esp_err_t audio_ret = audio_manager_start();
    if (audio_ret != ESP_OK) {
        return audio_ret;
    }

    esp_err_t arbiter_ret = audio_manager_playback_arbiter_init();
    if (arbiter_ret == ESP_OK) {
        arbiter_ret = audio_manager_playback_arbiter_start();
    }
    if (arbiter_ret != ESP_OK) {
        APP_LOGE(TAG, PLAYBACK_ARBITER_START_FAILED_2DE47B6D,
                 "Playback arbiter startup failed after audio READY: %s",
                 esp_err_to_name(arbiter_ret));
        return arbiter_ret;
    }

    arbiter_ret = audio_manager_capture_arbiter_init();
    if (arbiter_ret == ESP_OK) {
        arbiter_ret = audio_manager_capture_arbiter_start();
    }
    if (arbiter_ret != ESP_OK) {
        APP_LOGE(TAG, CAPTURE_ARBITER_START_FAILED_692CEE47,
                 "Capture arbiter startup failed after audio READY: %s",
                 esp_err_to_name(arbiter_ret));
        return arbiter_ret;
    }
    APP_LOGI(TAG, AUDIO_ARBITERS_READY_5E6F3494,
             "Audio arbiters READY playback=yes capture=yes");

    const esp_err_t voice_ret = voice_assistant_start_stack();
    if (voice_ret != ESP_OK) {
        APP_LOGE(TAG, VOICE_STACK_STARTUP_FAILED_2FE6294C,
                 "Voice stack startup failed after audio READY: %s",
                 esp_err_to_name(voice_ret));
        return voice_ret;
    }

    return ESP_OK;
}
