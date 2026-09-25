#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SENSOR_MANAGER_STATE_UNINITIALIZED = 0,
    SENSOR_MANAGER_STATE_INITIALIZED,
    SENSOR_MANAGER_STATE_RUNNING,
    SENSOR_MANAGER_STATE_READY,
    SENSOR_MANAGER_STATE_DEGRADED,
    SENSOR_MANAGER_STATE_ERROR,
} sensor_manager_state_t;

typedef struct {
    sensor_manager_state_t state;
    float temperature_c;
    float humidity_percent;
    bool data_valid;
    bool data_stale;
    int64_t last_success_time_ms;
} sensor_manager_status_t;
