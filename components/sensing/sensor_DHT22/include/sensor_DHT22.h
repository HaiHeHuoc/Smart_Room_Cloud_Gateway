#pragma once

#include "esp_err.h"

/** @brief One validated DHT22 temperature and humidity sample. */
typedef struct
{
    /** Temperature in degrees Celsius. */
    float temperature_c;

    /** Relative humidity percentage. */
    float humidity_percent;
} dht22_sensor_data_t;

/**
 * @brief Read and validate one sample from the board-configured DHT22.
 *
 * The output is updated only after both values pass finite-value and DHT22
 * operating-range checks.
 *
 * @param[out] data Destination for the validated sample.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if data is NULL,
 *         ESP_ERR_INVALID_RESPONSE for out-of-range data, or an error from the
 *         underlying DHT driver when communication fails.
 */
esp_err_t dht22_sensor_read(
    dht22_sensor_data_t *data);

/**
 * @brief Start the standalone periodic DHT22 bring-up logging task.
 *
 * @note This helper is intended for hardware bring-up. Do not run it together
 *       with sensor_manager, because both tasks would access the same sensor.
 */
void dht22_bringup_start(void);
