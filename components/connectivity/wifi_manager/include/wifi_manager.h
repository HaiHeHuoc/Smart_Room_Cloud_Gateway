#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IEEE 802.11 SSID maximum length is 32 bytes.
 * One additional byte is reserved for the null terminator.
 */
#define WIFI_MANAGER_SSID_MAX_LENGTH          32U
#define WIFI_MANAGER_SSID_BUFFER_SIZE         \
    (WIFI_MANAGER_SSID_MAX_LENGTH + 1U)

/*
 * WPA/WPA2 passphrase can contain up to 63 printable characters.
 * ESP-IDF also permits a 64-character hexadecimal PSK.
 */
#define WIFI_MANAGER_PASSWORD_MAX_LENGTH      64U
#define WIFI_MANAGER_PASSWORD_BUFFER_SIZE     \
    (WIFI_MANAGER_PASSWORD_MAX_LENGTH + 1U)

/*
 * Longest textual IPv4 representation:
 *
 *     "255.255.255.255" + '\0'
 */
#define WIFI_MANAGER_IPV4_STRING_SIZE         16U

/**
 * @brief Current Wi-Fi Station state.
 */
typedef enum
{
    WIFI_MANAGER_STATE_UNINITIALIZED = 0,
    WIFI_MANAGER_STATE_READY,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_WAITING_FOR_IP,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_DISCONNECTED,
    WIFI_MANAGER_STATE_FAILED,
    WIFI_MANAGER_STATE_RETRY_WAIT,
} wifi_manager_state_t;

/**
 * @brief Wi-Fi Station credentials.
 *
 * wifi_manager_connect() copies the credentials into the ESP-IDF Wi-Fi
 * configuration during the call. The caller does not need to preserve the
 * input pointers after the function returns.
 */
typedef struct
{
    /**
     * @brief Null-terminated Wi-Fi SSID.
     */
    const char *ssid;

    /**
     * @brief Null-terminated Wi-Fi password.
     *
     * An empty string is allowed for an open access point.
     */
    const char *password;
} wifi_manager_sta_config_t;

/**
 * @brief Snapshot of the current Wi-Fi connection status.
 */
typedef struct
{
    /**
     * @brief Current manager state.
     */
    wifi_manager_state_t state;

    /**
     * @brief SSID currently configured for the station.
     */
    char ssid[WIFI_MANAGER_SSID_BUFFER_SIZE];

    /**
     * @brief Current IPv4 address as text.
     *
     * Empty when the station has not received an IP address.
     */
    char ipv4_address[WIFI_MANAGER_IPV4_STRING_SIZE];

    /**
     * @brief Current access-point signal strength in dBm.
     *
     * The value is valid only when rssi_valid is true.
     */
    int8_t rssi_dbm;

    /**
     * @brief ESP-IDF Wi-Fi disconnect reason code.
     *
     * Zero means that no disconnect reason has been recorded.
     */
    uint16_t disconnect_reason;

    /**
     * @brief Whether ipv4_address contains a valid address.
     */
    bool has_ipv4_address;

    /**
     * @brief Whether rssi_dbm contains a valid RSSI reading.
     */
    bool rssi_valid;
} wifi_manager_status_t;

/**
 * @brief Wi-Fi status-change callback.
 *
 * Important:
 *
 * This callback can execute from the ESP event-loop task. It must finish
 * quickly and must not call LVGL directly.
 *
 * Recommended usage:
 *
 *     callback
 *         -> copy status into FreeRTOS queue
 *         -> return immediately
 *
 *     ui_task
 *         -> receive queue message
 *         -> update LVGL objects
 *
 * @param status    New Wi-Fi status snapshot.
 * @param user_data User context supplied during callback registration.
 */
typedef void (*wifi_manager_status_callback_t)(
    const wifi_manager_status_t *status,
    void *user_data
);


/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the Wi-Fi Station manager.
 *
 * Prerequisites:
 *
 *     1. NVS Flash is initialized.
 *     2. esp_netif_init() has completed.
 *     3. esp_event_loop_create_default() has completed.
 *
 * This function:
 *
 *     - creates the default Station network interface;
 *     - initializes the ESP-IDF Wi-Fi driver;
 *     - registers Wi-Fi and IP event handlers;
 *     - creates the reconnect task and connection-timeout timer;
 *     - sets Station mode;
 *     - leaves the manager in WIFI_MANAGER_STATE_READY.
 *
 * This function must be called only once.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - another ESP-IDF error code on failure
 */
esp_err_t wifi_manager_init(void);


/**
 * @brief Configure credentials and begin a timed connection to an access point.
 *
 * The supplied credentials are copied into the Wi-Fi driver's RAM storage.
 * The caller retains ownership of the input strings, while durable storage
 * remains exclusively owned by config_manager outside this component.
 *
 * @param config Station SSID/password configuration.
 *
 * @return
 *      - ESP_OK if connection startup was requested successfully
 *      - ESP_ERR_INVALID_ARG if credentials are invalid
 *      - ESP_ERR_INVALID_STATE if the manager is not initialized
 *      - another ESP-IDF error code on failure
 */
esp_err_t wifi_manager_connect(
    const wifi_manager_sta_config_t *config
);

/**
 * @brief Disconnect the station from the current access point.
 *
 * Automatic reconnect remains disabled until wifi_manager_connect() is called
 * again with a valid Station configuration.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the manager is not initialized
 *      - another ESP-IDF error code on failure
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Disconnect a Station connection not yet owned by wifi_manager.
 *
 * This is a narrow provisioning-cleanup operation. Call it only after the
 * provisioning framework has reached STOPPED and its final verified handoff
 * queue is empty. Unlike wifi_manager_disconnect(), it does not set persistent
 * manual-disconnect intent, so a later provisioning session may connect.
 *
 * The operation is rejected after credentials/reconnect ownership has been
 * configured or adopted. Completion of an active driver disconnect remains
 * asynchronous through WIFI_EVENT_STA_DISCONNECTED.
 *
 * @return ESP_OK when already inactive or when disconnect was requested,
 *         ESP_ERR_INVALID_STATE before initialization or for an owned runtime
 *         connection, or another ESP-IDF Wi-Fi error.
 */
esp_err_t wifi_manager_discard_unmanaged_connection(void);

/**
 * @brief Erase persistent Wi-Fi settings owned by the ESP-IDF driver.
 *
 * The provisioning framework temporarily uses WIFI_STORAGE_FLASH and can
 * leave a second credential copy outside config_manager. This operation calls
 * esp_wifi_restore() through the Wi-Fi owner so a factory-reset reboot cannot
 * reload that stale driver state.
 *
 * This function does not erase config_manager data, disconnect the current
 * Station connection, or reboot. Call it only from normal task context before
 * clearing the authoritative application Wi-Fi configuration. Reboot promptly
 * after the complete reset transaction succeeds.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization, or
 *         an ESP-IDF Wi-Fi error when persistent settings cannot be restored.
 */
esp_err_t wifi_manager_clear_persistent_driver_settings(void);

/**
 * @brief Copy the current Wi-Fi status.
 *
 * @param status Output status structure.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if status is NULL
 *      - ESP_ERR_INVALID_STATE if the manager is not initialized
 */
esp_err_t wifi_manager_get_status(
    wifi_manager_status_t *status
);

/**
 * @brief Read the current access-point RSSI.
 *
 * Internally this will use the connected Station access-point information.
 *
 * @param rssi_dbm Output RSSI value in dBm.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if rssi_dbm is NULL
 *      - ESP_ERR_INVALID_STATE if Wi-Fi is not connected
 *      - another ESP-IDF error code on failure
 */
esp_err_t wifi_manager_get_rssi(
    int8_t *rssi_dbm
);

/**
 * @brief Check whether wifi_manager owns an IPv4-ready Station connection.
 *
 * Connected means the Station has a valid IPv4 address and its credentials
 * have been configured or adopted by wifi_manager. A connection created by
 * provisioning remains not ready until wifi_manager_adopt_active_connection()
 * completes. Association while waiting for DHCP is not connected.
 *
 * @return true only while wifi_manager owns a valid IPv4 connection.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Register a callback for Wi-Fi status changes.
 *
 * Only one callback is supported in the initial implementation.
 * Registering a new callback replaces the previous callback.
 *
 * Passing NULL as callback unregisters the current callback.
 * Registering a non-NULL callback immediately invokes it once with the latest
 * status snapshot. The callback runs outside the manager critical section.
 *
 * @param callback  Callback function, or NULL to unregister.
 * @param user_data User context passed to the callback.
 *
 * @return ESP_OK.
 */
esp_err_t wifi_manager_register_status_callback(
    wifi_manager_status_callback_t callback,
    void *user_data
);

/**
 * @brief Convert a Wi-Fi manager state to readable text.
 *
 * Useful for logs and LVGL labels.
 *
 * @param state Wi-Fi manager state.
 *
 * @return Constant null-terminated state string.
 */
const char *wifi_manager_state_to_string(
    wifi_manager_state_t state
);

/**
 * @brief Scan nearby Wi-Fi access points and print SSID/RSSI information.
 *
 * This is a blocking function and must be called from a normal task.
 * Do not call it from a Wi-Fi/IP event callback.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if wifi_manager is not initialized
 *      - ESP_ERR_WIFI_STATE if Wi-Fi is currently connecting
 *      - ESP_ERR_NO_MEM if scan-result allocation fails
 *      - another ESP-IDF Wi-Fi error code
 */
esp_err_t wifi_manager_scan_and_log(void);

/**
 * @brief Adopt a Station connection established by provisioning.
 *
 * Restores Wi-Fi manager runtime policy and republishes the current CONNECTED
 * snapshot to the registered status callback.
 *
 * @return ESP_OK on success, or an ESP-IDF error when the current connection
 * is not eligible for adoption.
 */
esp_err_t wifi_manager_adopt_active_connection(void);

#ifdef __cplusplus
}
#endif
