#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Type Definitions --------------------------------------------------------- */
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
 * @brief Resolve persistent Wi-Fi policy and start the selected network path.
 *
 * This blocking boot-orchestration API is accepted only from READY. Stored
 * credentials start an asynchronous Station connection. An unconfigured
 * device runs the bounded BLE provisioning flow through connection adoption.
 *
 * @return ESP_OK when the selected path starts or completes successfully, or
 * an error from config_manager, provisioning_manager, or wifi_manager.
 */
esp_err_t app_network_coordinator_start(void);

#ifdef __cplusplus
}
#endif
