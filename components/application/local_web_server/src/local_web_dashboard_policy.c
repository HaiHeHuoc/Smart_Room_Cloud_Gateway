#include "local_web_dashboard_policy.h"

#include <math.h>

const char *local_web_dashboard_sensor_state_name(sensor_manager_state_t state)
{
    switch (state) {
        case SENSOR_MANAGER_STATE_UNINITIALIZED: return "uninitialized";
        case SENSOR_MANAGER_STATE_INITIALIZED: return "initialized";
        case SENSOR_MANAGER_STATE_RUNNING: return "running";
        case SENSOR_MANAGER_STATE_READY: return "ready";
        case SENSOR_MANAGER_STATE_DEGRADED: return "degraded";
        case SENSOR_MANAGER_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

const char *local_web_dashboard_storage_state_name(sd_card_manager_state_t state)
{
    switch (state) {
        case SD_CARD_MANAGER_STATE_UNINITIALIZED: return "uninitialized";
        case SD_CARD_MANAGER_STATE_INITIALIZING: return "initializing";
        case SD_CARD_MANAGER_STATE_MOUNTING: return "mounting";
        case SD_CARD_MANAGER_STATE_RETRY_WAIT: return "retry_wait";
        case SD_CARD_MANAGER_STATE_READY: return "ready";
        case SD_CARD_MANAGER_STATE_RECOVERING: return "recovering";
        case SD_CARD_MANAGER_STATE_UNAVAILABLE: return "unavailable";
        default: return "unknown";
    }
}

const char *local_web_dashboard_audio_state_name(audio_manager_state_t state)
{
    switch (state) {
        case AUDIO_MANAGER_STATE_UNINITIALIZED: return "uninitialized";
        case AUDIO_MANAGER_STATE_INITIALIZED: return "initialized";
        case AUDIO_MANAGER_STATE_IDLE: return "idle";
        case AUDIO_MANAGER_STATE_RECORDING: return "recording";
        case AUDIO_MANAGER_STATE_PROCESSING: return "processing";
        case AUDIO_MANAGER_STATE_PLAYBACK: return "playback";
        case AUDIO_MANAGER_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

const char *local_web_dashboard_cloud_state_name(cloud_manager_state_t state)
{
    switch (state) {
        case CLOUD_MANAGER_STATE_UNINITIALIZED: return "uninitialized";
        case CLOUD_MANAGER_STATE_INITIALIZED: return "initialized";
        case CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK: return "waiting_for_network";
        case CLOUD_MANAGER_STATE_WAITING_FOR_DATA: return "waiting_for_data";
        case CLOUD_MANAGER_STATE_UPLOADING: return "uploading";
        case CLOUD_MANAGER_STATE_ONLINE: return "online";
        case CLOUD_MANAGER_STATE_RETRY_WAIT: return "retry_wait";
        case CLOUD_MANAGER_STATE_AUTH_ERROR: return "auth_error";
        case CLOUD_MANAGER_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

const char *local_web_dashboard_network_state_name(wifi_manager_state_t state)
{
    switch (state) {
        case WIFI_MANAGER_STATE_UNINITIALIZED: return "uninitialized";
        case WIFI_MANAGER_STATE_READY: return "ready";
        case WIFI_MANAGER_STATE_CONNECTING: return "connecting";
        case WIFI_MANAGER_STATE_WAITING_FOR_IP: return "waiting_for_ip";
        case WIFI_MANAGER_STATE_CONNECTED: return "connected";
        case WIFI_MANAGER_STATE_DISCONNECTED: return "disconnected";
        case WIFI_MANAGER_STATE_FAILED: return "failed";
        case WIFI_MANAGER_STATE_RETRY_WAIT: return "retry_wait";
        default: return "unknown";
    }
}

const char *local_web_dashboard_time_state_name(time_manager_state_t state)
{
    switch (state) {
        case TIME_MANAGER_STATE_UNINITIALIZED: return "uninitialized";
        case TIME_MANAGER_STATE_INITIALIZED: return "initialized";
        case TIME_MANAGER_STATE_WAITING_NETWORK: return "waiting_for_network";
        case TIME_MANAGER_STATE_SYNCING: return "syncing";
        case TIME_MANAGER_STATE_SYNCED: return "synced";
        case TIME_MANAGER_STATE_RETRY_WAIT: return "retry_wait";
        case TIME_MANAGER_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

bool local_web_dashboard_sensor_has_current_data(
    const sensor_manager_status_t *status)
{
    return (status != NULL) && status->data_valid && !status->data_stale &&
           isfinite(status->temperature_c) && isfinite(status->humidity_percent);
}

uint64_t local_web_dashboard_age_ms(uint64_t now_ms, int64_t then_ms)
{
    if ((then_ms <= 0) || ((uint64_t)then_ms > now_ms)) {
        return 0U;
    }
    return now_ms - (uint64_t)then_ms;
}

local_web_dashboard_health_t local_web_dashboard_sensor_health(
    const sensor_manager_status_t *status)
{
    return ((status != NULL) &&
            (status->state == SENSOR_MANAGER_STATE_READY) &&
            local_web_dashboard_sensor_has_current_data(status))
               ? LOCAL_WEB_DASHBOARD_HEALTH_NORMAL
               : LOCAL_WEB_DASHBOARD_HEALTH_ATTENTION;
}

local_web_dashboard_health_t local_web_dashboard_storage_health(
    const sd_card_manager_status_t *status, bool capacity_valid)
{
    return ((status != NULL) &&
            (status->state == SD_CARD_MANAGER_STATE_READY) && capacity_valid)
               ? LOCAL_WEB_DASHBOARD_HEALTH_NORMAL
               : LOCAL_WEB_DASHBOARD_HEALTH_ATTENTION;
}

local_web_dashboard_health_t local_web_dashboard_audio_health(
    const audio_manager_status_t *status, bool playback_status_available)
{
    if ((status == NULL) || !playback_status_available ||
        (status->state == AUDIO_MANAGER_STATE_UNINITIALIZED) ||
        (status->state == AUDIO_MANAGER_STATE_ERROR)) {
        return LOCAL_WEB_DASHBOARD_HEALTH_ATTENTION;
    }
    return LOCAL_WEB_DASHBOARD_HEALTH_NORMAL;
}

local_web_dashboard_health_t local_web_dashboard_cloud_health(
    const cloud_manager_status_t *status)
{
    if ((status != NULL) &&
        ((status->state == CLOUD_MANAGER_STATE_ONLINE) ||
         (status->state == CLOUD_MANAGER_STATE_WAITING_FOR_DATA) ||
         (status->state == CLOUD_MANAGER_STATE_UPLOADING))) {
        return LOCAL_WEB_DASHBOARD_HEALTH_NORMAL;
    }
    return LOCAL_WEB_DASHBOARD_HEALTH_ATTENTION;
}

local_web_dashboard_health_t local_web_dashboard_network_health(
    const wifi_manager_status_t *status)
{
    return ((status != NULL) &&
            (status->state == WIFI_MANAGER_STATE_CONNECTED) &&
            status->has_ipv4_address)
               ? LOCAL_WEB_DASHBOARD_HEALTH_NORMAL
               : LOCAL_WEB_DASHBOARD_HEALTH_ATTENTION;
}

local_web_dashboard_health_t local_web_dashboard_time_health(
    const time_manager_status_t *status)
{
    return ((status != NULL) && status->synced)
               ? LOCAL_WEB_DASHBOARD_HEALTH_NORMAL
               : LOCAL_WEB_DASHBOARD_HEALTH_ATTENTION;
}

const char *local_web_dashboard_overall_state(
    const local_web_dashboard_health_t *health,
    size_t health_count)
{
    if ((health == NULL) || (health_count == 0U)) {
        return "unavailable";
    }

    size_t available_count = 0U;
    bool attention = false;
    for (size_t index = 0U; index < health_count; ++index) {
        if (health[index] != LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE) {
            ++available_count;
        }
        if (health[index] != LOCAL_WEB_DASHBOARD_HEALTH_NORMAL) {
            attention = true;
        }
    }
    return available_count == 0U ? "unavailable" :
           attention ? "attention" : "ready";
}
