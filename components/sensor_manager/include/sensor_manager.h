#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef _cplusplus
extern "C"
{
#endif

/* Types ------------------------------------------------------------------- */

/** @brief Lifecycle and data-quality state of the sensor manager. */
typedef enum
{
    /** sensor_manager_init() has not completed. */
    SENSOR_MANAGER_STATE_UNINITIALIZED = 0,

    /** Configuration and synchronization objects are ready. */
    SENSOR_MANAGER_STATE_INITIALIZED,

    /** The sampling task is running but has not produced a valid sample. */
    SENSOR_MANAGER_STATE_RUNNING,

    /** The latest read succeeded and the data is current. */
    SENSOR_MANAGER_STATE_READY,

    /** A read failed, but the last-known-good sample is still usable. */
    SENSOR_MANAGER_STATE_DEGRADED,

    /** No usable sample is available, or the retained sample is stale. */
    SENSOR_MANAGER_STATE_ERROR
} sensor_manager_state_t;

/** @brief Runtime sampling and stale-data limits. */
typedef struct
{
    /** Sampling interval in milliseconds; must be at least 2000 ms. */
    uint32_t sample_period_ms;

    /**
     * Age in milliseconds after which retained data is stale; must be greater
     * than sample_period_ms.
     */
    uint32_t stale_timeout_ms;
} sensor_manager_config_t;

/** @brief Thread-safe snapshot of sensor state, data, and read statistics. */
typedef struct
{
    /** Current manager state. */
    sensor_manager_state_t state;

    /** Last-known-good temperature in degrees Celsius. */
    float temperature_c;

    /** Last-known-good relative humidity percentage. */
    float humidity_percent;

    /** True after at least one valid DHT22 sample has been stored. */
    bool data_valid;

    /** True when no valid sample exists or the retained sample is too old. */
    bool data_stale;

    /** Result of the latest sensor read. */
    esp_err_t last_error;

    /** Number of successful reads since initialization. */
    uint32_t successful_read_count;

    /** Number of failed reads since initialization. */
    uint32_t failed_read_count;

    /** Number of failures since the latest successful read. */
    uint32_t consecutive_failure_count;

    /** esp_timer time of the latest valid sample, in milliseconds. */
    int64_t last_success_time_ms;
} sensor_manager_status_t;

/**
 * @brief Callback invoked after each periodic sensor read.
 *
 * The callback executes in the sensor manager task after the internal mutex
 * has been released. The status pointer refers to a temporary snapshot and
 * must not be retained after the callback returns.
 *
 * @param status Latest sensor status snapshot.
 * @param user_context Context supplied during callback registration.
 */
typedef void (*sensor_manager_status_callback_t)(
    const sensor_manager_status_t *status,
    void *user_context);

/* Public API --------------------------------------------------------------- */

/**
 * @brief Initialize the sensor manager without starting periodic sampling.
 *
 * @param config Sampling period and stale timeout copied by the manager.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid configuration,
 *         ESP_ERR_INVALID_STATE if already initialized, or ESP_ERR_NO_MEM if
 *         the status mutex cannot be created.
 */
esp_err_t sensor_manager_init(
    const sensor_manager_config_t *config);

/**
 * @brief Start the periodic DHT22 sampling task.
 *
 * Call sensor_manager_init() first. The task can be started only once.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE when initialization or
 *         lifecycle state is invalid, ESP_ERR_TIMEOUT if the status mutex
 *         cannot be acquired, or ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t sensor_manager_start(void);

/**
 * @brief Copy the current sensor status under the manager mutex.
 *
 * @param[out] status Destination for the status snapshot.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL,
 *         ESP_ERR_INVALID_STATE before initialization, or ESP_ERR_TIMEOUT if
 *         the status mutex cannot be acquired.
 */
esp_err_t sensor_manager_get_status(
    sensor_manager_status_t *status);

/**
 * @brief Register the application status callback before sampling starts.
 *
 * Registration is accepted only after sensor_manager_init() and before
 * sensor_manager_start(). The current implementation requires a non-NULL
 * callback and supports one callback/context pair.
 *
 * @param callback Callback invoked after each sensor read.
 * @param user_context Context passed back to the callback.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if callback is NULL, or
 *         ESP_ERR_INVALID_STATE outside the registration window.
 */
esp_err_t sensor_manager_register_callback(
    sensor_manager_status_callback_t callback,
    void *user_context);

#ifdef _cplusplus
}
#endif
