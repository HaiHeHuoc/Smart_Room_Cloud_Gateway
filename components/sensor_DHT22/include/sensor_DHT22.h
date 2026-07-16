#pragma once

#include "esp_err.h"

typedef struct
{
    float temperature_c;
    float humidity_percent;
} dht22_sensor_data_t;

esp_err_t dht22_sensor_read(
    dht22_sensor_data_t *data);

void dht22_bringup_start(void);