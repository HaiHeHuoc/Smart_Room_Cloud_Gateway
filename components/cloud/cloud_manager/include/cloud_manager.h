#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Type Definitions --------------------------------------------------------- */
/** @brief Cloud upload lifecycle and latest request result. */
typedef enum
{
    /** cloud_manager_init() has not completed. */
    CLOUD_MANAGER_STATE_UNINITIALIZED = 0,
    /** Configuration, queue, and status mutex are ready. */
    CLOUD_MANAGER_STATE_INITIALIZED,
    /** Telemetry is pending, but Wi-Fi has no IPv4 connection. */
    CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK,
    /** Wi-Fi is connected, but no telemetry snapshot is pending. */
    CLOUD_MANAGER_STATE_WAITING_FOR_DATA,
    /** The cloud task is authenticating or sending telemetry. */
    CLOUD_MANAGER_STATE_UPLOADING,
    /** The most recent Firebase upload succeeded. */
    CLOUD_MANAGER_STATE_ONLINE,
    /** A transport or retryable HTTP error is waiting for backoff. */
    CLOUD_MANAGER_STATE_RETRY_WAIT,
    /** Firebase Authentication or Database authorization failed. */
    CLOUD_MANAGER_STATE_AUTH_ERROR,
    /** A non-retryable request or configuration error occurred. */
    CLOUD_MANAGER_STATE_ERROR
} cloud_manager_state_t;

/** @brief Sensor snapshot copied into the cloud manager's latest-value queue. */
typedef struct
{
    /**
     * Temperature copied from sensor_manager.
     *
     * The value is serialized as supplied, including a finite failure
     * sentinel. Consumers must also inspect data_valid and data_stale.
     */
    float temperature_c;

    /**
     * Relative humidity copied from sensor_manager.
     *
     * The value is serialized as supplied, including a finite failure
     * sentinel. Consumers must also inspect data_valid and data_stale.
     */
    float humidity_percent;

    /** Whether sensor_manager has recorded a successful sample. */
    bool data_valid;

    /** Whether the latest successful sample has exceeded its stale timeout. */
    bool data_stale;

    /** Sensor manager state encoded for the Firebase payload. */
    int32_t sensor_state;
    /** Result of the most recent sensor read. */
    esp_err_t last_error;

    /** Uptime of the latest valid sensor sample, in milliseconds. */
    int64_t sample_uptime_ms;
} cloud_sensor_telemetry_t;

/** @brief Thread-safe snapshot of cloud state and upload statistics. */
typedef struct
{
    /** Current cloud manager state. */
    cloud_manager_state_t state;

    /** Result of the latest upload attempt. */
    esp_err_t last_error;
    /** HTTP status from the latest request, or zero before a response. */
    int last_http_status;

    /** Number of successful uploads since initialization. */
    uint32_t successful_upload_count;
    /** Number of failed upload attempts since initialization. */
    uint32_t failed_upload_count;
    /** Failures since the latest successful upload. */
    uint32_t consecutive_failure_count;

    /** Backoff delay selected for the next retry, in milliseconds. */
    uint32_t current_retry_delay_ms;

    /** Uptime of the latest successful upload, in milliseconds. */
    int64_t last_success_time_ms;
} cloud_manager_status_t;

/**
 * @brief Receive a copied cloud status snapshot after a state/result change.
 *
 * The callback runs in the cloud task context after the status mutex has been
 * released. Keep it non-blocking and copy or queue any data needed later.
 *
 * @param[in] status Temporary status snapshot valid only during the callback.
 * @param[in] user_data Opaque pointer supplied during registration.
 */
typedef void (*cloud_manager_status_callback_t)(
    const cloud_manager_status_t *status,
    void *user_data);

/** @brief Firebase endpoint and successful-upload pacing configuration. */
typedef struct
{
    /** Base `.json` URL without authentication or other query parameters. */
    const char *firebase_latest_url;

    /** Minimum delay between successful uploads, in milliseconds. */
    uint32_t publish_period_ms;
} cloud_manager_config_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Initialize cloud state, synchronization, and latest-value queue.
 *
 * This function copies the URL and configuration. It does not start a task or
 * perform a network request. The latest-value telemetry queue is ready when
 * this function returns ESP_OK, so initialize it before starting any producer
 * whose callback posts telemetry. Initialize firebase_auth and wifi_manager
 * before starting the cloud manager task.
 *
 * @param[in] config Firebase endpoint and publish period to copy.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid URL/period,
 *         ESP_ERR_INVALID_STATE if already initialized, or ESP_ERR_NO_MEM if
 *         synchronization or queue allocation fails.
 */
esp_err_t cloud_manager_init(
    const cloud_manager_config_t *config);

/**
 * @brief Start the single cloud upload task.
 *
 * When BLE provisioning shares constrained memory with TLS, the composition
 * root must defer this call until provisioning cleanup and network handoff
 * have completed.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization or
 *         when already started, or ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t cloud_manager_start(void);

/**
 * @brief Register or remove the cloud status callback.
 *
 * Only one callback is retained. Register after cloud_manager_init() and
 * before cloud_manager_start() when the initial task states are required.
 * Passing NULL unregisters the current callback.
 *
 * @param[in] callback Callback to retain, or NULL to unregister.
 * @param[in] user_data Opaque pointer passed to callback.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization, or
 *         ESP_ERR_TIMEOUT if the status mutex cannot be acquired.
 */
esp_err_t cloud_manager_register_status_callback(
    cloud_manager_status_callback_t callback,
    void *user_data);

/**
 * @brief Replace pending telemetry with the newest sensor snapshot.
 *
 * This non-blocking API copies the structure into a queue of length one. It is
 * suitable for the sensor callback and does not perform network I/O. Posting
 * is supported after cloud_manager_init() and before cloud_manager_start().
 *
 * @param[in] telemetry Snapshot to copy. Numeric values marked valid must be
 *            finite. The current payload still serializes finite values when
 *            data_valid is false or data_stale is true.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid input,
 *         ESP_ERR_INVALID_STATE before initialization, or ESP_FAIL if the
 *         queue update fails.
 */
esp_err_t cloud_manager_post_sensor_telemetry(
    const cloud_sensor_telemetry_t *telemetry);

/**
 * @brief Copy cloud state and statistics under the component mutex.
 *
 * @param[out] status Destination for the status snapshot.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL,
 *         ESP_ERR_INVALID_STATE before initialization, or ESP_ERR_TIMEOUT if
 *         the mutex cannot be acquired.
 */
esp_err_t cloud_manager_get_status(
    cloud_manager_status_t *status);

#ifdef __cplusplus
}
#endif
