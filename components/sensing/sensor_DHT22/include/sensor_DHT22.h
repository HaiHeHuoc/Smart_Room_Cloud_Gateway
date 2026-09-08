#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

/** @brief Runtime configuration copied by the DHT22 sensor component. */
typedef struct
{
    /** GPIO connected to the DHT22 data pin. */
    gpio_num_t gpio_num;
} dht22_sensor_config_t;

/** @brief One validated DHT22 temperature and humidity sample. */
typedef struct
{
    /** Temperature in degrees Celsius. */
    float temperature_c;

    /** Relative humidity percentage. */
    float humidity_percent;
} dht22_sensor_data_t;

/**
 * @brief Configure the DHT22 data GPIO used by subsequent reads.
 *
 * The configuration is copied by the component. Repeating initialization with
 * the same GPIO is idempotent; attempting to change GPIO after initialization
 * returns ESP_ERR_INVALID_STATE.
 *
 * @param[in] config DHT22 runtime configuration.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if config is NULL, or
 *         ESP_ERR_INVALID_STATE when already initialized with another GPIO.
 */
esp_err_t dht22_sensor_init(
    const dht22_sensor_config_t *config);

/**
 * @brief Read and validate one sample from the configured DHT22.
 *
 * Call dht22_sensor_init() before the first read. The output is updated only
 * after both values pass finite-value and DHT22 operating-range checks.
 *
 * @param[out] data Destination for the validated sample.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if data is NULL,
 *         ESP_ERR_INVALID_STATE before initialization,
 *         ESP_ERR_INVALID_RESPONSE for out-of-range data, or an error from the
 *         underlying DHT driver when communication fails.
 */
esp_err_t dht22_sensor_read(
    dht22_sensor_data_t *data);

/**
 * @brief Start the standalone periodic DHT22 bring-up logging task.
 *
 * dht22_sensor_init() must complete first. If the component is not initialized
 * this helper logs the invalid lifecycle request and does not create a task.
 *
 * @note This helper is intended for hardware bring-up. Do not run it together
 *       with sensor_manager, because both tasks would access the same sensor.
 */
void dht22_bringup_start(void);
