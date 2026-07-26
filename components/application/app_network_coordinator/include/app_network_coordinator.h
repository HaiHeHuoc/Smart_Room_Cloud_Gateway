#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Type Definitions --------------------------------------------------------- */

/**
 * @brief Runtime Wi-Fi event delivered by the application composition layer.
 */
typedef enum
{
    /**
     * @brief A Station connection or DHCP attempt is active.
     */
    APP_NETWORK_COORDINATOR_WIFI_EVENT_CONNECTING = 0,

    /**
     * @brief The Station owns a valid IPv4 address.
     */
    APP_NETWORK_COORDINATOR_WIFI_EVENT_ONLINE,

    /**
     * @brief The Station currently has no usable IPv4 connection.
     */
    APP_NETWORK_COORDINATOR_WIFI_EVENT_OFFLINE,
} app_network_coordinator_wifi_event_t;

/**
 * @brief Application-level network lifecycle state.
 *
 * These states describe the application's network readiness rather than the
 * internal lifecycle of wifi_manager or provisioning_manager.
 */
typedef enum
{
    APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED = 0,

    /**
     * @brief Coordinator is initialized but boot orchestration has not started.
     */
    APP_NETWORK_COORDINATOR_STATE_READY,

    /**
     * @brief Network boot sequence is being started.
     */
    APP_NETWORK_COORDINATOR_STATE_STARTING,

    /**
     * @brief Persistent Wi-Fi configuration is being inspected.
     */
    APP_NETWORK_COORDINATOR_STATE_RESOLVING_CONFIG,

    /**
     * @brief Device is accepting credentials through provisioning.
     */
    APP_NETWORK_COORDINATOR_STATE_PROVISIONING,

    /**
     * @brief Station connection is in progress.
     */
    APP_NETWORK_COORDINATOR_STATE_CONNECTING,

    /**
     * @brief Station has a valid IP address.
     */
    APP_NETWORK_COORDINATOR_STATE_ONLINE,

    /**
     * @brief The device was online but currently has no usable connection.
     */
    APP_NETWORK_COORDINATOR_STATE_OFFLINE,

    /**
     * @brief A non-recoverable orchestration failure occurred.
     */
    APP_NETWORK_COORDINATOR_STATE_FAILED,
} app_network_coordinator_state_t;

/**
 * @brief Timing configuration for application-level network orchestration.
 */
typedef struct
{
    /**
     * @brief Maximum time allowed for one provisioning session.
     */
    uint32_t provisioning_timeout_ms;

    /**
     * @brief Poll period used while waiting for asynchronous manager cleanup.
     *
     * This field will be removed later when the coordinator becomes fully
     * event-driven.
     */
    uint32_t provisioning_poll_period_ms;
} app_network_coordinator_config_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Initialize the application network coordinator.
 *
 * The configuration is copied. Initialization is accepted only from
 * APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED.
 *
 * @param[in] config Provisioning timeout and cleanup polling configuration.
 *
 * @return
 * - ESP_OK: Coordinator initialized and entered READY.
 * - ESP_ERR_INVALID_ARG: @p config is NULL or a timing value is zero.
 * - ESP_ERR_INVALID_STATE: Coordinator was already initialized.
 */
esp_err_t app_network_coordinator_init(
    const app_network_coordinator_config_t *config);

/**
 * @brief Copy the current application network state.
 *
 * @param[out] state Destination for the thread-safe state snapshot.
 *
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG when @p state is NULL.
 */
esp_err_t app_network_coordinator_get_state(
    app_network_coordinator_state_t *state);

/**
 * @brief Convert a coordinator state to a constant readable string.
 *
 * @param[in] state State to convert.
 *
 * @return Constant state name, or `"UNKNOWN"` for an unsupported value.
 */
const char *app_network_coordinator_state_to_string(
    app_network_coordinator_state_t state);

/**
 * @brief Schedule application network boot orchestration.
 *
 * This non-blocking API creates a dedicated one-shot FreeRTOS task. Persistent
 * configuration resolution, stored connection startup, and bounded BLE
 * provisioning run in that task.
 *
 * ESP_OK confirms only that the task was created. Inspect coordinator and
 * manager states for the eventual network result.
 *
 * @return ESP_OK when the task is scheduled, ESP_ERR_INVALID_STATE unless the
 *         coordinator is READY, or ESP_ERR_NO_MEM when task creation fails.
 */
esp_err_t app_network_coordinator_start(void);

/**
 * @brief Notify the coordinator of one runtime Wi-Fi state event.
 *
 * This function performs only a short state update and may be called from a
 * normal task-context Wi-Fi status callback. It does not block, allocate
 * memory, call Wi-Fi APIs, or invoke GUI code.
 *
 * Runtime Wi-Fi events are intentionally ignored while the coordinator is
 * provisioning, resolving configuration, or in a terminal failure state.
 * Provisioning becomes ONLINE only after persistence, BLE cleanup, and
 * wifi_manager connection adoption have completed.
 *
 * @param[in] event Runtime Wi-Fi event.
 *
 * @return
 * - ESP_OK: Event was processed or intentionally ignored for the current
 *   lifecycle state.
 * - ESP_ERR_INVALID_ARG: Event value is unsupported.
 */
esp_err_t app_network_coordinator_notify_wifi_event(
    app_network_coordinator_wifi_event_t event);

#ifdef __cplusplus
}
#endif
