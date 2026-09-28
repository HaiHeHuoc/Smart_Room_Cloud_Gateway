#include "voice_assistant.h"

#include "audio_manager_capture_arbiter.h"
#include "audio_manager_playback_arbiter.h"
#include "board_config.h"
#include "esp_log.h"
#include "app_log.h"
#include "voice_assistant_audio_adapter.h"
#include "voice_assistant_downlink.h"
#include "voice_assistant_playback_control.h"
#include "voice_assistant_ptt.h"
#include "voice_assistant_ptt_gpio.h"
#include "voice_assistant_ui_gui_adapter.h"
#include "voice_assistant_ui_model.h"
#include "voice_assistant_uplink.h"
#include "voice_assistant_bootstrap_lifecycle.h"
#include "xiaozhi_foundation.h"

_Static_assert(PTT_BUTTON_USE_INTERNAL_PULLDOWN == 1,
               "Voice PTT GPIO contract requires internal pull-down");

static const char *const TAG = "VOICE_BOOTSTRAP";

static audio_manager_status_callback_t s_app_audio_callback = NULL;
static void *s_app_audio_callback_context = NULL;
static bool s_voice_started = false;
static bool s_bootstrap_retry_safe = true;

static esp_err_t bootstrap_audio_start(void *context)
{
    (void)context;
    return audio_manager_start();
}

static esp_err_t bootstrap_audio_rollback(void *context)
{
    (void)context;
    const esp_err_t stop_ret = audio_manager_stop();
    if (stop_ret != ESP_OK) {
        return stop_ret;
    }
    const esp_err_t observer_ret = audio_manager_register_status_callback(NULL, NULL);
    if (observer_ret != ESP_OK) {
        return observer_ret;
    }
    return audio_manager_deinit();
}

static esp_err_t bootstrap_playback_arbiter_start(void *context)
{
    (void)context;
    esp_err_t ret = audio_manager_playback_arbiter_init();
    return (ret == ESP_OK) ? audio_manager_playback_arbiter_start() : ret;
}

static esp_err_t bootstrap_playback_arbiter_rollback(void *context)
{
    (void)context;
    return audio_manager_playback_arbiter_stop_and_deinit();
}

static esp_err_t bootstrap_capture_arbiter_start(void *context)
{
    (void)context;
    esp_err_t ret = audio_manager_capture_arbiter_init();
    return (ret == ESP_OK) ? audio_manager_capture_arbiter_start() : ret;
}

static esp_err_t bootstrap_capture_arbiter_rollback(void *context)
{
    (void)context;
    return audio_manager_capture_arbiter_stop_and_deinit();
}

static esp_err_t bootstrap_voice_start(void *context)
{
    (void)context;
    esp_err_t ret = voice_assistant_init();
    return (ret == ESP_OK) ? voice_assistant_start() : ret;
}

static esp_err_t bootstrap_voice_rollback(void *context)
{
    (void)context;
    return voice_assistant_stop_and_deinit();
}

static esp_err_t bootstrap_ui_model_start(void *context)
{
    (void)context;
    esp_err_t ret = voice_assistant_ui_model_init();
    return (ret == ESP_OK) ? voice_assistant_ui_model_start() : ret;
}

static esp_err_t bootstrap_ui_model_rollback(void *context)
{
    (void)context;
    return voice_assistant_ui_model_stop_and_deinit();
}

static esp_err_t bootstrap_ui_adapter_start(void *context)
{
    (void)context;
    esp_err_t ret = voice_assistant_ui_gui_adapter_init();
    return (ret == ESP_OK) ? voice_assistant_ui_gui_adapter_start() : ret;
}

static esp_err_t bootstrap_ui_adapter_rollback(void *context)
{
    (void)context;
    return voice_assistant_ui_gui_adapter_stop_and_deinit();
}

static esp_err_t bootstrap_playback_control_start(void *context)
{
    (void)context;
    return voice_assistant_playback_control_init();
}

static esp_err_t bootstrap_playback_control_rollback(void *context)
{
    (void)context;
    return voice_assistant_playback_control_deinit();
}

static esp_err_t bootstrap_ptt_start(void *context)
{
    (void)context;
    esp_err_t ret = voice_assistant_ptt_init();
    return (ret == ESP_OK) ? voice_assistant_ptt_start() : ret;
}

static esp_err_t bootstrap_ptt_rollback(void *context)
{
    (void)context;
    return voice_assistant_ptt_stop_and_deinit();
}

static esp_err_t bootstrap_uplink_start(void *context)
{
    (void)context;
    esp_err_t ret = voice_assistant_uplink_init();
    return (ret == ESP_OK) ? voice_assistant_uplink_start() : ret;
}

static esp_err_t bootstrap_uplink_rollback(void *context)
{
    (void)context;
    return voice_assistant_uplink_stop_and_deinit();
}

static esp_err_t bootstrap_downlink_start(void *context)
{
    (void)context;
    esp_err_t ret = voice_assistant_downlink_init();
    return (ret == ESP_OK) ? voice_assistant_downlink_start() : ret;
}

static esp_err_t bootstrap_downlink_rollback(void *context)
{
    (void)context;
    return voice_assistant_downlink_stop_and_deinit();
}

static esp_err_t bootstrap_gpio_start(void *context)
{
    (void)context;
    const voice_assistant_ptt_gpio_config_t config = {
        .gpio_num = PTT_BUTTON_GPIO,
        .active_level = PTT_BUTTON_ACTIVE_LEVEL,
        .poll_period_ms = PTT_BUTTON_POLL_PERIOD_MS,
        .debounce_ms = PTT_BUTTON_DEBOUNCE_MS,
    };
    esp_err_t ret = voice_assistant_ptt_gpio_init(&config);
    return (ret == ESP_OK) ? voice_assistant_ptt_gpio_start() : ret;
}

static esp_err_t bootstrap_gpio_rollback(void *context)
{
    (void)context;
    return voice_assistant_ptt_gpio_stop_and_deinit();
}

static esp_err_t bootstrap_session_start(void *context)
{
    (void)context;
    return voice_assistant_begin_session();
}

static esp_err_t bootstrap_session_rollback(void *context)
{
    (void)context;
    xiaozhi_foundation_session_status_t status = {0};
    const esp_err_t status_ret = xiaozhi_foundation_session_get_status(&status);
    return ((status_ret == ESP_OK) && status.active)
               ? xiaozhi_foundation_session_stop()
               : ((status_ret == ESP_OK) ? ESP_OK : status_ret);
}

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

esp_err_t voice_assistant_start_after_audio_ready(
    audio_manager_status_callback_t application_callback,
    void *application_callback_context)
{
    if (s_voice_started) {
        return ESP_OK;
    }
    if (!s_bootstrap_retry_safe) {
        return ESP_ERR_INVALID_STATE;
    }
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

    static const voice_assistant_bootstrap_stage_t stages[] = {
        { "audio_manager", bootstrap_audio_start, bootstrap_audio_rollback },
        { "playback_arbiter", bootstrap_playback_arbiter_start, bootstrap_playback_arbiter_rollback },
        { "capture_arbiter", bootstrap_capture_arbiter_start, bootstrap_capture_arbiter_rollback },
        { "voice_task", bootstrap_voice_start, bootstrap_voice_rollback },
        { "ui_model", bootstrap_ui_model_start, bootstrap_ui_model_rollback },
        { "ui_adapter", bootstrap_ui_adapter_start, bootstrap_ui_adapter_rollback },
        { "playback_control", bootstrap_playback_control_start, bootstrap_playback_control_rollback },
        { "ptt", bootstrap_ptt_start, bootstrap_ptt_rollback },
        { "uplink", bootstrap_uplink_start, bootstrap_uplink_rollback },
        { "downlink", bootstrap_downlink_start, bootstrap_downlink_rollback },
        { "gpio38_ptt", bootstrap_gpio_start, bootstrap_gpio_rollback },
        { "xiaozhi_session", bootstrap_session_start, bootstrap_session_rollback },
    };
    voice_assistant_bootstrap_lifecycle_t lifecycle = {0};
    voice_assistant_bootstrap_lifecycle_init(&lifecycle, stages,
                                             sizeof(stages) / sizeof(stages[0]));
    esp_err_t rollback_ret = ESP_OK;
    const esp_err_t startup_ret = voice_assistant_bootstrap_lifecycle_start(
        &lifecycle, NULL, &rollback_ret);
    if (startup_ret != ESP_OK) {
        APP_LOGE(TAG, VOICE_STACK_STARTUP_FAILED_2FE6294C,
                 "Bootstrap stage failed error=%s rollback=%s retry=%s",
                 esp_err_to_name(startup_ret), esp_err_to_name(rollback_ret),
                 lifecycle.safe_for_retry ? "allowed" : "blocked");
        s_app_audio_callback = NULL;
        s_app_audio_callback_context = NULL;
        s_bootstrap_retry_safe = lifecycle.safe_for_retry;
        return startup_ret;
    }
    s_voice_started = true;
    s_bootstrap_retry_safe = false;
    APP_LOGI(TAG, VOICE_STACK_READY_AFTER_AUDIO_35809038,
             "Voice stack READY; bootstrap stages=%u ptt_gpio=%d",
             (unsigned)(sizeof(stages) / sizeof(stages[0])), (int)PTT_BUTTON_GPIO);
    return ESP_OK;
}

bool voice_assistant_bootstrap_retry_is_safe(void)
{
    return s_bootstrap_retry_safe;
}
