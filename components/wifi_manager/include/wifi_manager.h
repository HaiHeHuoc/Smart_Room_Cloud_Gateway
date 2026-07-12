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
    /**
     * @brief wifi_manager_init() has not been called.
     */
    WIFI_MANAGER_STATE_UNINITIALIZED = 0,

    /**
     * @brief Wi-Fi driver is initialized and ready.
     */
    WIFI_MANAGER_STATE_READY,

    /**
     * @brief Station is trying to associate with the access point.
     */
    WIFI_MANAGER_STATE_CONNECTING,

    /**
     * @brief Station is associated and waiting for DHCP/IP.
     */
    WIFI_MANAGER_STATE_WAITING_FOR_IP,

    /**
     * @brief Station received an IPv4 address.
     */
    WIFI_MANAGER_STATE_CONNECTED,

    /**
     * @brief Station was disconnected from the access point.
     */
    WIFI_MANAGER_STATE_DISCONNECTED,

    /**
     * @brief Connection attempt failed.
     */
    WIFI_MANAGER_STATE_FAILED,
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
 * @brief Configure credentials and begin connecting to an access point.
 *
 * This first Sprint 2 implementation uses hardcoded credentials supplied
 * by the application. NVS-based credentials will be introduced in Sprint 5.
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
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the manager is not initialized
 *      - another ESP-IDF error code on failure
 */
esp_err_t wifi_manager_disconnect(void);

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
 * @brief Check whether the Station has received an IP address.
 *
 * @return true if the manager state is WIFI_MANAGER_STATE_CONNECTED.
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Register a callback for Wi-Fi status changes.
 *
 * Only one callback is supported in the initial implementation.
 * Registering a new callback replaces the previous callback.
 *
 * Passing NULL as callback unregisters the current callback.
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

#ifdef __cplusplus
}
#endif