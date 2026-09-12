#include "xiaozhi_system_status_composition.h"

#include <stdio.h>

#include "audio_manager.h"
#include "cloud_manager.h"
#include "sd_card_manager.h"
#include "sensor_manager.h"
#include "time_manager.h"
#include "xiaozhi_foundation.h"

static const char *app_xiaozhi_system_sensor_state(
    const sensor_manager_status_t *status,
    bool *normal)
{
    *normal = (status->state == SENSOR_MANAGER_STATE_READY) &&
        status->data_valid && !status->data_stale;
    return *normal ? "ready" : "attention";
}

static const char *app_xiaozhi_system_cloud_state(
    const cloud_manager_status_t *status,
    bool *normal)
{
    *normal = (status->state == CLOUD_MANAGER_STATE_ONLINE) ||
        (status->state == CLOUD_MANAGER_STATE_WAITING_FOR_DATA) ||
        (status->state == CLOUD_MANAGER_STATE_UPLOADING);
    return *normal ? "ready" : "attention";
}

static const char *app_xiaozhi_system_time_state(
    const time_manager_status_t *status,
    bool *normal)
{
    *normal = status->synced;
    return *normal ? "synced" : "attention";
}

static const char *app_xiaozhi_system_storage_state(
    const sd_card_manager_status_t *status,
    bool *normal)
{
    *normal = (status->state == SD_CARD_MANAGER_STATE_READY);
    return *normal ? "ready" : "attention";
}

static const char *app_xiaozhi_system_audio_state(
    const audio_manager_status_t *status,
    bool *normal)
{
    switch (status->state) {
        case AUDIO_MANAGER_STATE_INITIALIZED:
        case AUDIO_MANAGER_STATE_IDLE:
            *normal = true;
            return "idle";
        case AUDIO_MANAGER_STATE_RECORDING:
        case AUDIO_MANAGER_STATE_PROCESSING:
        case AUDIO_MANAGER_STATE_PLAYBACK:
            *normal = true;
            return "busy";
        case AUDIO_MANAGER_STATE_UNINITIALIZED:
        case AUDIO_MANAGER_STATE_ERROR:
        default:
            *normal = false;
            return "attention";
    }
}

static void app_xiaozhi_system_copy_token(
    char *destination,
    size_t destination_size,
    const char *token)
{
    (void)snprintf(destination, destination_size, "%s", token);
}

static esp_err_t app_xiaozhi_copy_system_status(
    xiaozhi_foundation_system_status_query_snapshot_t *snapshot,
    void *user_context)
{
    (void)user_context;
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *snapshot = (xiaozhi_foundation_system_status_query_snapshot_t){0};
    uint32_t available_count = 0U;
    uint32_t attention_count = 0U;
    bool normal = false;

    sensor_manager_status_t sensor = {0};
    if (sensor_manager_get_status(&sensor) == ESP_OK) {
        app_xiaozhi_system_copy_token(snapshot->sensor_state,
                                      sizeof(snapshot->sensor_state),
                                      app_xiaozhi_system_sensor_state(&sensor, &normal));
        ++available_count;
        attention_count += normal ? 0U : 1U;
    } else {
        app_xiaozhi_system_copy_token(snapshot->sensor_state,
                                      sizeof(snapshot->sensor_state), "unavailable");
        ++attention_count;
    }

    cloud_manager_status_t cloud = {0};
    if (cloud_manager_get_status(&cloud) == ESP_OK) {
        app_xiaozhi_system_copy_token(snapshot->cloud_state,
                                      sizeof(snapshot->cloud_state),
                                      app_xiaozhi_system_cloud_state(&cloud, &normal));
        ++available_count;
        attention_count += normal ? 0U : 1U;
    } else {
        app_xiaozhi_system_copy_token(snapshot->cloud_state,
                                      sizeof(snapshot->cloud_state), "unavailable");
        ++attention_count;
    }

    time_manager_status_t time = {0};
    if (time_manager_get_status(&time) == ESP_OK) {
        app_xiaozhi_system_copy_token(snapshot->time_state,
                                      sizeof(snapshot->time_state),
                                      app_xiaozhi_system_time_state(&time, &normal));
        ++available_count;
        attention_count += normal ? 0U : 1U;
    } else {
        app_xiaozhi_system_copy_token(snapshot->time_state,
                                      sizeof(snapshot->time_state), "unavailable");
        ++attention_count;
    }

    sd_card_manager_status_t storage = {0};
    if (sd_card_manager_get_status(&storage) == ESP_OK) {
        app_xiaozhi_system_copy_token(snapshot->storage_state,
                                      sizeof(snapshot->storage_state),
                                      app_xiaozhi_system_storage_state(&storage, &normal));
        ++available_count;
        attention_count += normal ? 0U : 1U;
    } else {
        app_xiaozhi_system_copy_token(snapshot->storage_state,
                                      sizeof(snapshot->storage_state), "unavailable");
        ++attention_count;
    }

    audio_manager_status_t audio = {0};
    if (audio_manager_get_status(&audio) == ESP_OK) {
        app_xiaozhi_system_copy_token(snapshot->audio_state,
                                      sizeof(snapshot->audio_state),
                                      app_xiaozhi_system_audio_state(&audio, &normal));
        ++available_count;
        attention_count += normal ? 0U : 1U;
    } else {
        app_xiaozhi_system_copy_token(snapshot->audio_state,
                                      sizeof(snapshot->audio_state), "unavailable");
        ++attention_count;
    }

    snapshot->available = true;
    app_xiaozhi_system_copy_token(
        snapshot->overall_state, sizeof(snapshot->overall_state),
        (available_count == 0U) ? "unavailable" :
        (attention_count == 0U) ? "ready" : "attention");
    return ESP_OK;
}

esp_err_t app_xiaozhi_system_status_query_register_provider(void)
{
    return xiaozhi_foundation_register_system_status_query_provider(
        app_xiaozhi_copy_system_status,
        NULL);
}
