#pragma once

#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    const char *firebase_latest_url;
    uint32_t publish_period_ms;
} cloud_manager_config_t;

esp_err_t cloud_manager_init(
    const cloud_manager_config_t *config);

esp_err_t cloud_manager_start(void);

esp_err_t cloud_manager_post_sensor_telemetry(
    const cloud_sensor_telemetry_t *telemetry);

#ifdef __cplusplus
}
#endif