#pragma once

/* Includes ----------------------------------------------------------------- */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief Lifecycle states owned by the BLE provisioning manager.
 *
 * State transitions are protected internally and may be read safely from
 * another task with provisioning_manager_get_state().
 */
typedef enum
{
    /**
     * @brief provisioning_manager_init() has not completed successfully.
     */
    PROVISIONING_MANAGER_STATE_UNINITIALIZED = 0,

    /**
     * @brief Manager is initialized and ready to start provisioning.
     */
    PROVISIONING_MANAGER_STATE_READY,

    /**
     * @brief Provisioning startup is in progress.
     */
    PROVISIONING_MANAGER_STATE_STARTING,

    /**
     * @brief BLE provisioning is active and accepting a client connection.
     */
    PROVISIONING_MANAGER_STATE_ACTIVE,

    /**
     * @brief Provisioning shutdown and resource cleanup are in progress.
     */
    PROVISIONING_MANAGER_STATE_STOPPING,

    /**
     * @brief Provisioning has stopped and released its runtime resources.
     *
     * Phase 6.1 treats this as a terminal state for the current boot.
     */
    PROVISIONING_MANAGER_STATE_STOPPED,

    /**
     * @brief Initialization, startup, task creation, or cleanup failed.
     */
    PROVISIONING_MANAGER_STATE_FAILED,
} provisioning_manager_state_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Initialize the BLE provisioning framework and internal state.
 *
 * Initialization is idempotent after a successful call. A lifecycle that has
 * entered PROVISIONING_MANAGER_STATE_FAILED is not silently reinitialized.
 * The application must initialize the default ESP event loop and required
 * Wi-Fi infrastructure before calling this function.
 *
 * @return
 * - ESP_OK: Manager is initialized or was already initialized successfully.
 * - ESP_ERR_INVALID_STATE: Initialization is already running or the manager
 *   is in the terminal FAILED state.
 * - Other ESP-IDF errors returned by the provisioning framework.
 */
esp_err_t provisioning_manager_init(void);

/**
 * @brief Start the BLE provisioning service.
 *
 * The service uses Security 1 and a MAC-derived service name. This operation
 * is valid only while the manager is in READY state. The READY-to-STARTING
 * transition is atomic, preventing duplicate concurrent starts.
 *
 * @return
 * - ESP_OK: BLE provisioning started successfully.
 * - ESP_ERR_INVALID_STATE: The manager is not in READY state.
 * - Other ESP-IDF errors returned while creating or starting the service.
 */
esp_err_t provisioning_manager_start(void);

/**
 * @brief Request asynchronous provisioning shutdown and resource cleanup.
 *
 * This operation is valid only in ACTIVE state. Completion is asynchronous:
 * NETWORK_PROV_END schedules de-initialization outside the framework callback,
 * and the lifecycle reaches STOPPED after NETWORK_PROV_DEINIT.
 *
 * @return
 * - ESP_OK: The stop request was accepted.
 * - ESP_ERR_INVALID_STATE: The manager is not in ACTIVE state.
 */
esp_err_t provisioning_manager_stop(void);

/**
 * @brief Copy the current lifecycle state.
 *
 * This API is thread-safe and does not block on provisioning events.
 *
 * @param[out] state Destination for the current state.
 *
 * @return
 * - ESP_OK: State copied successfully.
 * - ESP_ERR_INVALID_ARG: @p state is NULL.
 */
esp_err_t provisioning_manager_get_state(
    provisioning_manager_state_t *state);

#ifdef __cplusplus
}
#endif
