#include "audio_manager_named_playback.h"

#include "audio_manager_arbitration.h"
#include "audio_manager_playback_arbiter.h"

static esp_err_t submit_named_wav(
    uint32_t request_id,
    audio_manager_client_t client,
    const char *path)
{
    if ((request_id == 0U) || (path == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_manager_request_t request = {0};
    esp_err_t ret = audio_manager_request_make_default(
        request_id,
        client,
        AUDIO_MANAGER_RESOURCE_PLAYBACK,
        &request);
    if (ret != ESP_OK) {
        return ret;
    }

    return audio_manager_playback_arbiter_submit_wav(&request, path);
}

esp_err_t audio_manager_play_notification_wav(
    uint32_t request_id,
    const char *path)
{
    return submit_named_wav(
        request_id,
        AUDIO_MANAGER_CLIENT_NOTIFICATION,
        path);
}

esp_err_t audio_manager_play_critical_alarm_wav(
    uint32_t request_id,
    const char *path)
{
    return submit_named_wav(
        request_id,
        AUDIO_MANAGER_CLIENT_ALARM,
        path);
}
