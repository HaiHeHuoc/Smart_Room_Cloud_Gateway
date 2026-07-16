#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef _cplusplus
extern "C"
{
#endif

typedef enum
{
    SENSOR_MANAGER_STATE_UNINITIALIZED = 0,
    SENSOR_MANAGER_STATE_INITIALIZED,
    SENSOR_MANAGER_STATE_RUNNING,
    SENSOR_MANAGER_STATE_READY,
    SENSOR_MANAGER_STATE_DEGRADED,
    SENSOR_MANAGER_STATE_ERROR
} sensor_manager_state_t;

typedef struct
{
    uint32_t sample_period_ms;
    uint32_t stale_timeout_ms;
} sensor_manager_config_t;


typedef struct
{
    sensor_manager_state_t state;

    float temperature_c;
    float humidity_percent;

    bool data_valid;
    bool data_stale;

    esp_err_t last_error;

    uint32_t successful_read_count;
    uint32_t failed_read_count;
    uint32_t consecutive_failure_count;

    int64_t last_success_time_ms;
} sensor_manager_status_t;

esp_err_t sensor_manager_init(
    const sensor_manager_config_t *config);

esp_err_t sensor_manager_start(void);

esp_err_t sensor_manager_get_status(
    sensor_manager_status_t *status);

#ifdef _cplusplus
}
#endif
