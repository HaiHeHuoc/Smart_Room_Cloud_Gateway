#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Configuration Limits ---------------------------------------------------- */

/** Maximum copied SNTP server name size, including the null terminator. */
#define TIME_MANAGER_SNTP_SERVER_MAX_BYTES  64U

/** Maximum copied POSIX timezone string size, including the null terminator. */
#define TIME_MANAGER_TIMEZONE_MAX_BYTES     64U

/** Minimum buffer size for YYYY-MM-DDTHH:MM:SS+07:00 plus null terminator. */
#define TIME_MANAGER_ISO8601_BUFFER_SIZE    26U

/* Type Definitions --------------------------------------------------------- */

/** @brief Time synchronization lifecycle state. */
typedef enum
{
    /** time_manager_init() has not completed. */
    TIME_MANAGER_STATE_UNINITIALIZED = 0,

    /** Configuration and timezone are ready; SNTP has not started. */
    TIME_MANAGER_STATE_INITIALIZED,

    /** SNTP is configured but the application has not reported valid IPv4. */
    TIME_MANAGER_STATE_WAITING_NETWORK,

    /** SNTP is running and has not yet reported a successful synchronization. */
    TIME_MANAGER_STATE_SYNCING,

    /** The ESP-IDF SNTP callback has synchronized the system clock. */
    TIME_MANAGER_STATE_SYNCED,

    /** Reserved for a future explicit component retry policy. */
    TIME_MANAGER_STATE_RETRY_WAIT,

    /** Initialization or SNTP startup failed. */
    TIME_MANAGER_STATE_ERROR
} time_manager_state_t;

/** @brief Caller-provided configuration copied during initialization. */
typedef struct
{
    /** NUL-terminated SNTP server host name. */
    const char *sntp_server;

    /** NUL-terminated POSIX TZ string used by local-time conversions. */
    const char *timezone;
} time_manager_config_t;

/** @brief Thread-safe snapshot of synchronization state and diagnostics. */
typedef struct
{
    /** Current lifecycle/synchronization state. */
    time_manager_state_t state;

    /** True after at least one successful ESP-IDF SNTP synchronization. */
    bool synced;

    /** Most recent component startup or synchronization error. */
    esp_err_t last_error;

    /** Unix time recorded when SNTP last synchronized the system clock. */
    time_t last_sync_unix;

    /** Number of successful SNTP synchronization callbacks observed. */
    uint32_t sync_count;

    /** Most recently reported valid IPv4 state; this does not prove Internet access. */
    bool network_available;
} time_manager_status_t;

/**
 * @brief Receive a copied time-manager status snapshot after a state change.
 *
 * The callback runs in task context. A successful SNTP update invokes it from
 * ESP-IDF's TCP/IP task; lifecycle notifications run in the task that called
 * time_manager_start() or time_manager_notify_network_state(). The status
 * pointer is temporary and must not be retained. Keep the callback
 * non-blocking and do not call LVGL directly.
 *
 * @param status Temporary status snapshot.
 * @param user_context Context supplied during registration.
 */
typedef void (*time_manager_status_callback_t)(
    const time_manager_status_t *status,
    void *user_context);

/* Public API --------------------------------------------------------------- */

/**
 * @brief Return the default Google Public NTP and Vietnam timezone settings.
 *
 * The returned pointers refer to static literals. time_manager_init() copies
 * their contents, so callers may also pass short-lived configuration strings.
 */
time_manager_config_t time_manager_default_config(void);

/**
 * @brief Initialize configuration, timezone handling, and status storage.
 *
 * This function does not access the network or start SNTP. It can be called
 * once only and must run in task context.
 *
 * @param config Non-NULL configuration with bounded, non-empty strings.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid configuration,
 *         ESP_ERR_INVALID_SIZE for an overlong string, ESP_ERR_INVALID_STATE
 *         when already initializing/initialized, or an error from timezone
 *         setup.
 */
esp_err_t time_manager_init(
    const time_manager_config_t *config);

/**
 * @brief Configure ESP-NETIF SNTP and wait for a valid IPv4 notification.
 *
 * The ESP-IDF network stack must be initialized before this call. The function
 * never waits for an Internet connection or an SNTP response. On success the
 * lifecycle enters TIME_MANAGER_STATE_WAITING_NETWORK; the application must
 * later call time_manager_notify_network_state(true) to start SNTP. A
 * successful synchronization is reported through the registered
 * callback/status. Task context only.
 *
 * @return ESP_OK when SNTP was configured, ESP_ERR_INVALID_STATE before
 *         initialization or for a duplicate concurrent/successful start, or
 *         an ESP-NETIF SNTP error.
 */
esp_err_t time_manager_start(void);

/**
 * @brief Report whether the application currently has a valid IPv4 address.
 *
 * This is an application-composition input, normally forwarded from the
 * Wi-Fi manager status callback. true means an IPv4 address was acquired; it
 * does not claim that DNS or Internet reachability was proven. The first true
 * notification starts ESP-NETIF SNTP. A later false notification preserves a
 * previously synchronized system clock while the lifecycle returns to
 * TIME_MANAGER_STATE_WAITING_NETWORK. A subsequent true notification safely
 * restarts the configured ESP-NETIF SNTP service for an early resync.
 *
 * The function does not wait for DNS resolution or an SNTP packet/response.
 * It must run in task context, not ISR context. It does not call LVGL.
 *
 * @param has_ipv4_address Most recently observed IPv4 readiness.
 * @return ESP_OK on a handled notification, ESP_ERR_INVALID_STATE before
 *         successful time_manager_start(), or an ESP-NETIF SNTP start error.
 */
esp_err_t time_manager_notify_network_state(
    bool has_ipv4_address);

/**
 * @brief Register or remove the single application status callback.
 *
 * Passing NULL unregisters the callback. The callback is never invoked while
 * the internal status lock is held. Unregistration does not wait for a
 * callback snapshot selected concurrently, so application code must keep its
 * context valid until it has synchronized with any in-flight callback.
 *
 * @param callback Callback to register, or NULL to unregister.
 * @param user_context Context returned to a non-NULL callback.
 * @return ESP_OK on success or ESP_ERR_INVALID_STATE before initialization.
 */
esp_err_t time_manager_register_status_callback(
    time_manager_status_callback_t callback,
    void *user_context);

/**
 * @brief Copy the current synchronization status.
 *
 * @param[out] status Destination for the owned status snapshot.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL, or
 *         ESP_ERR_INVALID_STATE before initialization.
 */
esp_err_t time_manager_get_status(
    time_manager_status_t *status);

/**
 * @brief Return whether the system clock has been synchronized by SNTP.
 *
 * This is safe to call from task context and returns false before
 * initialization or before the first successful synchronization. Once true,
 * it remains true across an IPv4 loss because the synchronized system clock
 * continues locally.
 */
bool time_manager_is_synced(void);

/**
 * @brief Read the synchronized ESP-IDF system Unix time.
 *
 * @param[out] unix_time Destination for the current Unix timestamp.
 * @return ESP_OK after synchronization, including while offline after a
 *         prior sync; ESP_ERR_INVALID_ARG if unix_time is NULL,
 *         ESP_ERR_INVALID_STATE before a successful synchronization, or
 *         ESP_FAIL if the C runtime cannot read system time.
 */
esp_err_t time_manager_get_unix_time(
    time_t *unix_time);

/**
 * @brief Read synchronized local time using the configured POSIX timezone.
 *
 * @param[out] local_time Destination for a reentrant local-time conversion.
 * @return ESP_OK after synchronization, ESP_ERR_INVALID_ARG if local_time is
 *         NULL, ESP_ERR_INVALID_STATE before synchronization, or ESP_FAIL if
 *         the C runtime conversion fails.
 */
esp_err_t time_manager_get_local_time(
    struct tm *local_time);

/**
 * @brief Format synchronized local time as YYYY-MM-DDTHH:MM:SS+07:00.
 *
 * The UTC offset is derived from the configured timezone's local and UTC
 * conversions; it is not manually adjusted by a fixed number of hours.
 *
 * @param[out] buffer Destination string buffer.
 * @param buffer_size Size of buffer in bytes; at least
 *                    TIME_MANAGER_ISO8601_BUFFER_SIZE is required.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL buffer,
 *         ESP_ERR_INVALID_SIZE for insufficient storage,
 *         ESP_ERR_INVALID_STATE before synchronization, or ESP_FAIL if a C
 *         runtime time conversion fails.
 */
esp_err_t time_manager_format_iso8601(
    char *buffer,
    size_t buffer_size);

#ifdef __cplusplus
}
#endif
