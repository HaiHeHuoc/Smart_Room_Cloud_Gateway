#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Macros ------------------------------------------------------------------- */
/** Maximum supported Wi-Fi SSID length, excluding the null terminator. */
#define PROVISIONING_MANAGER_WIFI_SSID_MAX_LEN 32U

/** Maximum supported Wi-Fi password length, excluding the null terminator. */
#define PROVISIONING_MANAGER_WIFI_PASSWORD_MAX_LEN 63U

/** Storage size for a null-terminated Wi-Fi SSID. */
#define PROVISIONING_MANAGER_WIFI_SSID_BUFFER_SIZE \
    (PROVISIONING_MANAGER_WIFI_SSID_MAX_LEN + 1U)

/** Storage size for a null-terminated Wi-Fi password. */
#define PROVISIONING_MANAGER_WIFI_PASSWORD_BUFFER_SIZE \
    (PROVISIONING_MANAGER_WIFI_PASSWORD_MAX_LEN + 1U)

/** Caller-owned storage required for a provisioning QR JSON payload. */
#define PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE 192U

#ifdef __cplusplus
extern "C" {
#endif

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief Application-owned copy of provisioned Wi-Fi credentials.
 *
 * This structure never references memory owned by the provisioning framework.
 * The caller must clear it promptly after persistence or on any error path.
 */
typedef struct
{
    /** Null-terminated Wi-Fi network name. */
    char ssid[PROVISIONING_MANAGER_WIFI_SSID_BUFFER_SIZE];

    /** Null-terminated Wi-Fi password, or an empty string for an open network. */
    char password[PROVISIONING_MANAGER_WIFI_PASSWORD_BUFFER_SIZE];
} provisioning_manager_wifi_credentials_t;

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
     * Framework cleanup has fully completed. A new session generation may
     * reinitialize from this state until Bluetooth memory is terminally
     * released.
     */
    PROVISIONING_MANAGER_STATE_STOPPED,

    /**
     * @brief Initialization, startup, task creation, or cleanup failed.
     */
    PROVISIONING_MANAGER_STATE_FAILED,
} provisioning_manager_state_t;

/**
 * @brief Non-sensitive progress events emitted by the provisioning manager.
 *
 * These events describe BLE/framework lifecycle facts. Application policy,
 * persistence, screen routing, and normal Wi-Fi ownership remain outside this
 * component.
 */
typedef enum
{
    PROVISIONING_MANAGER_PROGRESS_STARTING = 0,
    PROVISIONING_MANAGER_PROGRESS_WAITING_FOR_PHONE,
    PROVISIONING_MANAGER_PROGRESS_CREDENTIAL_RECEIVED,
    PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTING,
    PROVISIONING_MANAGER_PROGRESS_WIFI_CREDENTIAL_FAILED,
    PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTED,
    PROVISIONING_MANAGER_PROGRESS_STOPPING,
    PROVISIONING_MANAGER_PROGRESS_STOPPED,
    PROVISIONING_MANAGER_PROGRESS_FAILED,
} provisioning_manager_progress_t;

/**
 * @brief Copied, non-sensitive provisioning progress snapshot.
 */
typedef struct
{
    /** Non-zero identity of the provisioning session that emitted this event. */
    uint32_t session_generation;

    provisioning_manager_progress_t progress;
    esp_err_t last_error;
    uint16_t wifi_failure_reason;
} provisioning_manager_progress_status_t;

/**
 * @brief Task-context callback for provisioning progress.
 *
 * The callback receives a copied snapshot and is invoked outside the manager's
 * critical section. It must return promptly, must not call LVGL, and must not
 * retain the supplied pointer.
 */
typedef void (*provisioning_manager_progress_callback_t)(
    const provisioning_manager_progress_status_t *status,
    void *user_data);

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Register the single provisioning progress callback.
 *
 * Register before starting provisioning. Re-registering the same callback and
 * context is idempotent. Passing NULL unregisters the current callback.
 *
 * @param[in] callback Callback to register, or NULL to unregister.
 * @param[in] user_data Opaque context returned with each callback.
 *
 * @return ESP_OK on success, or ESP_ERR_INVALID_STATE when a different
 *         callback is already registered.
 */
esp_err_t provisioning_manager_register_progress_callback(
    provisioning_manager_progress_callback_t callback,
    void *user_data);

/**
 * @brief Initialize the BLE provisioning framework and internal state.
 *
 * Initialization is idempotent for the same generation while READY. A clean
 * STOPPED lifecycle can be reinitialized for a new non-zero generation. The
 * retained credential queue is reset before the framework is initialized for
 * that next session. A lifecycle that has entered
 * PROVISIONING_MANAGER_STATE_FAILED is not silently reinitialized.
 * The application must initialize the default ESP event loop and required
 * Wi-Fi infrastructure before calling this function.
 *
 * @param[in] session_generation Non-zero monotonically increasing session
 *                               identity owned by the application.
 *
 * @return
 * - ESP_OK: Manager is initialized or was already initialized successfully.
 * - ESP_ERR_INVALID_ARG: @p session_generation is zero.
 * - ESP_ERR_INVALID_STATE: Initialization is already running, the manager is
 *   not UNINITIALIZED/STOPPED/READY, or the manager is terminal FAILED.
 * - Other ESP-IDF errors returned by the provisioning framework.
 */
esp_err_t provisioning_manager_init(
    uint32_t session_generation);

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
 * @brief Copy the QR payload for the currently active BLE service.
 *
 * The payload contains the exact service name, Security 1 PoP, and transport
 * used by provisioning_manager_start(). It therefore contains sensitive
 * onboarding material and must not be logged. The caller owns the returned
 * copy and should clear it promptly after use.
 *
 * This API is thread-safe, does not call LVGL, and never returns an internal
 * pointer.
 *
 * @param[in] session_generation Expected non-zero active session identity.
 * @param[out] payload Destination for the null-terminated JSON payload.
 * @param[in] payload_size Destination size. It must be at least
 *                        PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE.
 *
 * @return
 * - ESP_OK: Active payload copied and null-terminated.
 * - ESP_ERR_INVALID_ARG: The generation is zero, @p payload is NULL, or the
 *   buffer is undersized.
 * - ESP_ERR_INVALID_STATE: No valid payload exists for the expected session.
 */
esp_err_t provisioning_manager_get_qr_payload(
    uint32_t session_generation,
    char *payload,
    size_t payload_size);

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
 * @brief Permanently release retained BLE/NimBLE memory.
 *
 * Phase 6.4.5 retains Bluetooth memory across clean STOPPED-to-READY session
 * retries. The coordinator calls this once, only after the complete retry
 * envelope has ended and no same-boot provisioning session can follow.
 *
 * This operation is valid only from STOPPED, is idempotent after success, and
 * makes later provisioning reinitialization unavailable until reboot.
 *
 * For the ESP32-S3 BLE-only configuration this calls
 * esp_bt_mem_release(ESP_BT_MODE_BLE) only after framework deinitialization.
 * ESP_ERR_NOT_FOUND is treated as an already-released success. Other failures
 * are counted for diagnostics and returned to the caller, which must treat
 * memory reclamation as best-effort rather than a network result.
 *
 * @return ESP_OK on success/already released, ESP_ERR_INVALID_STATE unless the
 *         manager is cleanly STOPPED, or an ESP-IDF Bluetooth error.
 */
esp_err_t provisioning_manager_release_ble_memory(void);

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

/**
 * @brief Check whether a credential-to-connection handoff is in progress.
 *
 * The result becomes true after valid credentials are received and remains
 * true while the framework connects, during an explicitly armed post-STOPPED
 * late handoff, or until verified credentials are consumed by
 * provisioning_manager_receive_wifi_credentials(). A failed connection or
 * explicit late discard clears it.
 *
 * This API returns only progress metadata and never exposes credential data.
 * It is thread-safe and does not block on provisioning events.
 *
 * @param[out] handoff_pending Destination for the progress snapshot.
 *
 * @return
 * - ESP_OK: Progress copied successfully.
 * - ESP_ERR_INVALID_ARG: @p handoff_pending is NULL.
 * - ESP_ERR_INVALID_STATE: Credential handoff is not initialized.
 */
esp_err_t provisioning_manager_is_wifi_handoff_pending(
    bool *handoff_pending);

/**
 * @brief Retain the active session's unverified credentials across teardown.
 *
 * This narrow recovery API is used only when the normal provisioning session
 * and its IPv4 grace have expired while a credential handoff is still
 * pending. It binds the retained RAM-only credential copy to
 * @p session_generation so asynchronous framework cleanup can preserve it for
 * one bounded post-STOPPED DHCP reconciliation window.
 *
 * No credential becomes application-visible and nothing is persisted by this
 * operation. Calling it for the same generation is idempotent. It also
 * succeeds when the handoff has concurrently advanced to the verified queue,
 * allowing the caller to reconcile that queue after teardown.
 *
 * @param[in] session_generation Expected non-zero active session identity.
 *
 * @return
 * - ESP_OK: Late handoff retention was armed or verification already won the
 *   boundary race.
 * - ESP_ERR_INVALID_ARG: @p session_generation is zero.
 * - ESP_ERR_INVALID_STATE: The generation does not match, no handoff is
 *   pending, or the lifecycle cannot be reconciled safely.
 */
esp_err_t provisioning_manager_arm_late_wifi_handoff(
    uint32_t session_generation);

/**
 * @brief Promote a retained late handoff after independent IPv4 verification.
 *
 * Valid only after provisioning cleanup has reached STOPPED. The supplied
 * connected SSID is compared exactly with the generation-bound pending SSID.
 * On a match, the retained credential copy is moved to the same verified queue
 * consumed by provisioning_manager_receive_wifi_credentials(). The password
 * remains internal and RAM-only until the application consumes that queue.
 *
 * This function must be called from task context, not from an ISR. The caller
 * must serialize it with factory-reset preparation through the application
 * reset-exclusion policy.
 *
 * @param[in] session_generation Expected non-zero retained session identity.
 * @param[in] connected_ssid Null-terminated SSID reported by the Wi-Fi owner
 *                           for the Station connection that has IPv4.
 *
 * @return
 * - ESP_OK: SSID matched and credentials were queued as verified.
 * - ESP_ERR_INVALID_ARG: An argument is invalid or the SSID is malformed.
 * - ESP_ERR_INVALID_STATE: No matching STOPPED late handoff is retained.
 * - ESP_ERR_INVALID_RESPONSE: The connected SSID does not match the retained
 *   credential set.
 * - ESP_FAIL: The verified queue could not accept the credential copy.
 */
esp_err_t provisioning_manager_confirm_late_wifi_handoff(
    uint32_t session_generation,
    const char *connected_ssid);

/**
 * @brief Securely discard one retained generation-bound late handoff.
 *
 * This operation zeroizes only an explicitly armed, unverified RAM credential
 * copy. It is valid during or after teardown so timeout, cleanup-error, and
 * factory-reset paths can remove sensitive data without waiting for another
 * framework event. Verified queue data remains owned by
 * provisioning_manager_receive_wifi_credentials() and must be drained
 * separately.
 *
 * @param[in] session_generation Expected non-zero retained session identity.
 *
 * @return ESP_OK when the matching retained copy was zeroized,
 *         ESP_ERR_INVALID_ARG for generation zero, or ESP_ERR_INVALID_STATE
 *         when no matching armed late handoff exists.
 */
esp_err_t provisioning_manager_discard_late_wifi_handoff(
    uint32_t session_generation);

/**
 * @brief Wait for and copy the latest provisioned Wi-Fi credentials.
 *
 * Credentials are deep-copied when received but become available through this
 * API only after the provisioning framework reports a successful Wi-Fi
 * connection. Failed connection attempts discard their pending credential
 * copy. The output is cleared before waiting and remains cleared on failure.
 *
 * This function must be called from task context, not from an ISR.
 *
 * @param[out] credentials Destination credential structure.
 * @param[in] timeout_ms Maximum time to wait in milliseconds. Zero performs
 * a non-blocking check.
 *
 * @return
 * - ESP_OK: Credentials were copied successfully.
 * - ESP_ERR_INVALID_ARG: credentials is NULL.
 * - ESP_ERR_INVALID_STATE: Credential handoff is not initialized.
 * - ESP_ERR_TIMEOUT: No credentials arrived before timeout.
 */
esp_err_t provisioning_manager_receive_wifi_credentials(
    provisioning_manager_wifi_credentials_t *credentials,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
