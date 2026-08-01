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
     * @brief Station association completed and DHCP is still pending.
     */
    APP_NETWORK_COORDINATOR_WIFI_EVENT_WAITING_FOR_IP,

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
     * @brief Post-session grace for a pending credential handoff to obtain IPv4.
     *
     * This window starts only when the normal provisioning session deadline is
     * reached with a handoff still pending. On expiry the coordinator performs
     * final verified-queue drains, quiesces the provisioning framework, and
     * allows one fixed five-second post-STOPPED late-DHCP settle before
     * classifying the attempt or starting a replacement session.
     */
    uint32_t provisioning_connection_grace_ms;

    /**
     * @brief Poll period used for cleanup, late DHCP, and reset-safe waits.
     *
     * This field will be removed later when the coordinator becomes fully
     * event-driven.
     */
    uint32_t provisioning_poll_period_ms;

    /**
     * @brief Maximum provisioning sessions, including the initial session.
     */
    uint32_t provisioning_max_sessions;

    /**
     * @brief Delay before starting each automatic replacement session.
     */
    uint32_t provisioning_retry_backoff_ms;

    /**
     * @brief Minimum terminal TIMEOUT or FAILED presentation dwell.
     *
     * A deadline path may already have quiesced the framework before this
     * dwell in order to close a concurrent GOT_IP race safely.
     */
    uint32_t provisioning_failure_dwell_ms;
} app_network_coordinator_config_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Initialize the application network coordinator.
 *
 * The configuration is copied. Initialization is accepted only from
 * APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED.
 *
 * @param[in] config Session timeout, pending-handoff post-session IPv4 grace,
 *                   cleanup polling, retry budget/backoff, and failure dwell.
 *
 * @return
 * - ESP_OK: Coordinator initialized and entered READY.
 * - ESP_ERR_INVALID_ARG: @p config is NULL, a value is zero, or a configured
 *   retry/timing bound is exceeded.
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
 * provisioning run in that task. For stored credentials, the task also keeps
 * BOOT visible for at most 60 seconds before requesting WIFI_STATUS when the
 * device is still offline; wifi_manager continues reconnecting independently.
 *
 * ESP_OK confirms only that the task was created. Inspect coordinator and
 * manager states for the eventual network result.
 *
 * @return ESP_OK when the task is scheduled, ESP_ERR_INVALID_STATE unless the
 *         coordinator is READY, or ESP_ERR_NO_MEM when task creation fails.
 */
esp_err_t app_network_coordinator_start(void);

/**
 * @brief Quiesce network provisioning before a factory-reset transaction.
 *
 * This synchronous task-context API atomically prevents new provisioning
 * session starts and credential handoff claims, then waits for any operation
 * already holding the reset-exclusion claim to finish. An ACTIVE provisioning
 * service is asked to stop, and STARTING/STOPPING lifecycle transitions are
 * polled with a finite delay until the framework can no longer produce a new
 * credential handoff. The Wi-Fi owner then disables automatic reconnect,
 * requests Station disconnect, and the same deadline covers confirmation that
 * the Station is detached.
 *
 * ESP_OK is a terminal handoff to the reset coordinator: the reset gate stays
 * asserted until reboot. The caller may then clear driver-owned persistence
 * and application Wi-Fi configuration. If preparation fails, a gate claimed
 * by that call is rolled back; this function never erases configuration,
 * calls persistent Wi-Fi reset APIs, allocates resources, creates a task, or
 * calls LVGL.
 *
 * The API is not ISR-safe and must not be called concurrently. The
 * app_reset_coordinator task is its sole application owner.
 *
 * @param[in] timeout_ms Non-zero total preparation deadline. It must convert
 *                       to at least one FreeRTOS tick and must not exceed the
 *                       coordinator's supported timing bound.
 *
 * @return
 * - ESP_OK: Provisioning cannot start or produce another credential handoff
 *   before reboot, any retained late handoff has been securely discarded, no
 *   credential persistence transaction is active, and the Station is detached
 *   with automatic reconnect disabled.
 * - ESP_ERR_INVALID_ARG: @p timeout_ms is zero, unrepresentable, or too large.
 * - ESP_ERR_INVALID_STATE: Coordinator is not initialized or safe lifecycle
 *   proof is unavailable.
 * - ESP_ERR_TIMEOUT: The reset-exclusion claim, provisioning cleanup, or
 *   Station detach did not become safe before the deadline.
 * - Other ESP-IDF errors returned while inspecting/stopping provisioning or
 *   quiescing the Station.
 */
esp_err_t app_network_coordinator_prepare_for_factory_reset(
    uint32_t timeout_ms);

/**
 * @brief Notify the coordinator of one runtime Wi-Fi state event.
 *
 * This function performs only a short state update and may be called from a
 * normal task-context Wi-Fi status callback. It does not block, allocate
 * memory, call Wi-Fi APIs, or call LVGL. A verified normal transition to
 * ONLINE may enqueue a non-blocking app_gui screen request.
 *
 * While provisioning, events are translated only into non-blocking GUI
 * progress updates. They cannot promote application state to ONLINE, request
 * the normal Wi-Fi screen, start cloud work, or enable reconnect ownership.
 * Provisioning becomes ONLINE only after persistence, BLE cleanup, and
 * wifi_manager connection adoption have completed.
 *
 * @param[in] event Runtime Wi-Fi event.
 * @param[in] disconnect_reason Raw Wi-Fi disconnect reason for OFFLINE, or
 *                              zero for other events.
 *
 * @return
 * - ESP_OK: Event was processed or intentionally ignored for the current
 *   lifecycle state.
 * - ESP_ERR_INVALID_ARG: Event value is unsupported.
 */
esp_err_t app_network_coordinator_notify_wifi_event(
    app_network_coordinator_wifi_event_t event,
    uint16_t disconnect_reason);

#ifdef __cplusplus
}
#endif
