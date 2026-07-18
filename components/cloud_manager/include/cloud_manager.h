#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    CLOUD_MANAGER_STATE_UNINITIALIZED = 0,
    CLOUD_MANAGER_STATE_INITIALIZED,
    CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK,
    CLOUD_MANAGER_STATE_WAITING_FOR_DATA,
    CLOUD_MANAGER_STATE_UPLOADING,
    CLOUD_MANAGER_STATE_ONLINE,
    CLOUD_MANAGER_STATE_RETRY_WAIT,
    CLOUD_MANAGER_STATE_AUTH_ERROR,
    CLOUD_MANAGER_STATE_ERROR
} cloud_manager_state_t;

typedef struct
{
    float temperature_c;
    float humidity_percent;

    bool data_valid;
    bool data_stale;

    int32_t sensor_state;
    esp_err_t last_error;

    int64_t sample_uptime_ms;
} cloud_sensor_telemetry_t;

typedef struct
{
    cloud_manager_state_t state;

    esp_err_t last_error;
    int last_http_status;

    uint32_t successful_upload_count;
    uint32_t failed_upload_count;
    uint32_t consecutive_failure_count;

    uint32_t current_retry_delay_ms;

    int64_t last_success_time_ms;
} cloud_manager_status_t;

typedef struct
{
    const char *firebase_latest_url;
    uint32_t publish_period_ms;
} cloud_manager_config_t;

esp_err_t cloud_manager_init(
    const cloud_manager_config_t *config);

esp_err_t cloud_manager_start(void);

esp_err_t cloud_manager_post_sensor_telemetry(
    const cloud_sensor_telemetry_t *telemetry);

esp_err_t cloud_manager_get_status(
    cloud_manager_status_t *status);

#ifdef __cplusplus
}
#endif