#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Telemetry Limits --------------------------------------------------------- */
/** ISO-8601 local time (YYYY-MM-DDTHH:MM:SS+07:00) plus null terminator. */
#define CLOUD_TIME_LOCAL_ISO8601_BUFFER_SIZE  26U

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

/** @brief Non-sensitive classification of the latest failed cloud attempt. */
typedef enum
{
    /** No failure has been recorded, or the latest upload succeeded. */
    CLOUD_MANAGER_FAILURE_NONE = 0,
    /** The attempt could not continue because IPv4 was unavailable. */
    CLOUD_MANAGER_FAILURE_WAIT_FOR_NETWORK,
    /** DNS, socket, TLS, timeout, or other transport work may be retried. */
    CLOUD_MANAGER_FAILURE_RETRYABLE_TRANSPORT,
    /** HTTP 408, 429, or 5xx may be retried with backoff. */
    CLOUD_MANAGER_FAILURE_RETRYABLE_HTTP,
    /** HTTP 401/403 requires a bounded token recovery attempt. */
    CLOUD_MANAGER_FAILURE_REAUTH_REQUIRED,
    /** Firebase credentials or authorization remain rejected. */
    CLOUD_MANAGER_FAILURE_AUTH_FATAL,
    /** Request configuration or serialization is deterministically invalid. */
    CLOUD_MANAGER_FAILURE_NONRETRYABLE_CONFIG,
    /** An internal lifecycle or resource error must not be retried. */
    CLOUD_MANAGER_FAILURE_NONRETRYABLE_INTERNAL
} cloud_manager_failure_class_t;

/** @brief Cloud-owned audio state used by the Firebase telemetry schema. */
typedef enum
{
    CLOUD_AUDIO_STATE_UNAVAILABLE = 0,
    CLOUD_AUDIO_STATE_READY,
    CLOUD_AUDIO_STATE_IDLE,
    CLOUD_AUDIO_STATE_RECORDING,
    CLOUD_AUDIO_STATE_PROCESSING,
    CLOUD_AUDIO_STATE_PLAYBACK,
    CLOUD_AUDIO_STATE_ERROR
} cloud_audio_state_t;

/** @brief Latest audio snapshot copied into the cloud telemetry payload. */
typedef struct
{
    /** Current project-owned audio state mapped by the application layer. */
    cloud_audio_state_t state;

    /** Result of the most recently completed audio operation. */
    esp_err_t last_error;
} cloud_audio_telemetry_t;

/** @brief Time snapshot owned by the cloud-facing Firebase schema. */
typedef struct
{
    /** True after the application has mapped a valid SNTP-synchronized clock. */
    bool synced;

    /** Current Unix time at application telemetry composition, or zero. */
    time_t unix_time;

    /** Local ISO-8601 time copied by the application, or an empty string. */
    char local_time[CLOUD_TIME_LOCAL_ISO8601_BUFFER_SIZE];

    /** Unix time of the most recent successful SNTP correction, or zero. */
    time_t last_sync_unix;
} cloud_time_telemetry_t;

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

    /** Latest audio manager state sampled by the application layer. */
    cloud_audio_telemetry_t audio;

    /** Latest synchronized time mapped by the application layer. */
    cloud_time_telemetry_t time;
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

    /** Uptime when the latest upload attempt started, in milliseconds. */
    int64_t last_attempt_time_ms;
    /** Uptime of the latest successful upload, in milliseconds. */
    int64_t last_success_time_ms;

    /** Number of reusable HTTP clients discarded after initialization. */
    uint32_t http_client_reset_count;
    /** Non-zero generation of the latest observed online/offline state. */
    uint32_t network_epoch;
    /** Number of accepted forced ID-token invalidations. */
    uint32_t auth_recovery_count;

    /** Classification of the latest failed attempt. */
    cloud_manager_failure_class_t last_failure_class;
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
 * @brief Publish the latest IPv4-ready state to the cloud task.
 *
 * Call this from the existing task-context Wi-Fi status callback. The function
 * performs no allocation, network operation, or wait. An online/offline edge
 * advances a non-zero network epoch and wakes the cloud task when it exists.
 * State and epoch changes are retained after cloud_manager_init() even before
 * cloud_manager_start().
 *
 * @param[in] has_ipv4_address true only while Station owns a usable IPv4
 *                             address.
 * @return ESP_OK when retained, or ESP_ERR_INVALID_STATE before
 *         cloud_manager_init().
 */
esp_err_t cloud_manager_notify_network_state(
    bool has_ipv4_address);

/**
 * @brief Replace pending telemetry with the newest sensor/audio/time snapshot.
 *
 * This non-blocking API copies the structure into a queue of length one. It is
 * suitable for the sensor callback, wakes the cloud task when present, and
 * does not perform network I/O. Posting is supported after
 * cloud_manager_init() and before cloud_manager_start().
 *
 * @param[in] telemetry Snapshot to copy. Numeric values marked valid must be
 *            finite, audio.state must be defined, and time must be either a
 *            complete synchronized snapshot or the zero/empty unsynchronized
 *            representation.
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
