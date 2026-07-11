#include "wifi_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
static const char *const TAG = "WIFI_MANAGER";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/**
 * @brief Internal state owned by wifi_manager.
 *
 * This structure is private. Other components can only obtain a copy through
 * wifi_manager_get_status().
 */
typedef struct
{
    bool initialized;

    wifi_manager_status_t status;

    wifi_manager_status_callback_t status_callback;

    void *status_callback_user_data;
} wifi_manager_context_t;

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static wifi_manager_context_t s_wifi_manager = {
    .initialized = false,

    .status = {
        .state = WIFI_MANAGER_STATE_UNINITIALIZED,
        .ssid = {0},
        .ipv4_address = {0},
        .rssi_dbm = 0,
        .disconnect_reason = 0U,
        .has_ipv4_address = false,
        .rssi_valid = false,
    },

    .status_callback = NULL,
    .status_callback_user_data = NULL,
};


/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */

esp_err_t wifi_manager_init(void)
{
    if(s_wifi_manager.initialized == true)
    {
        ESP_LOGW(TAG, "Wi-Fi manager is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "wifi_manager_init() is not implemented yet");

    /*
     * The real initialization flow will be implemented in Sprint 2.3:
     *
     * 1. Create default Wi-Fi Station network interface.
     * 2. Initialize Wi-Fi driver.
     * 3. Register Wi-Fi event handler.
     * 4. Register IP event handler.
     * 5. Set WIFI_MODE_STA.
     *
     * For now this is intentionally a compile-only skeleton.
     */

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_connect(
    const wifi_manager_sta_config_t *config
)
{
    ESP_RETURN_ON_FALSE(config != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Station configuration is NULL"
    );

    ESP_RETURN_ON_FALSE(config->ssid != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Wifi SSID is NULL"
    );

    ESP_RETURN_ON_FALSE(config->password != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Wifi password is NULL"
    );

    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    ESP_LOGW(TAG, "wifi_manager_connect() is not implemented yet");

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    ESP_LOGW(TAG, "wifi_manager_disconnect() is not implemented yet");

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wifi_manager_get_status(
    wifi_manager_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Output status pointer is NULL"
    );

    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    /*
     * A synchronization mechanism will be added when the Wi-Fi event handler
     * starts modifying the state from the ESP event-loop task.
     */
    memcpy(
        status,
        &s_wifi_manager.status,
        sizeof(*status)
    );

    return ESP_OK;
}

esp_err_t wifi_manager_get_rssi(
    int8_t *rssi_dbm)
{
    ESP_RETURN_ON_FALSE(rssi_dbm != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "RSSI output pointer is NULL"
    );

    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    ESP_RETURN_ON_FALSE(s_wifi_manager.status.state == WIFI_MANAGER_STATE_CONNECTED, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi is not connected"
    );

    /*
     * The real esp_wifi_sta_get_rssi() call will be added after the Station
     * connection flow is working.
     */

    ESP_LOGW(TAG, "wifi_manager_get_rssi() is not implemented yet");

    return ESP_ERR_NOT_SUPPORTED;
}

bool wifi_manager_is_connected(void)
{
    return s_wifi_manager.initialized && (
        s_wifi_manager.status.state == WIFI_MANAGER_STATE_CONNECTED
    );
}

esp_err_t wifi_manager_register_status_callback(
    wifi_manager_status_callback_t callback,
    void *user_data
)
{
    s_wifi_manager.status_callback = callback;
    s_wifi_manager.status_callback_user_data = user_data;

    ESP_LOGI(
        TAG,
        "Status callback %s",
        callback != NULL
            ? "registered"
            : "unregistered"
    );

    return ESP_OK;
}

const char *wifi_manager_state_to_string(
    wifi_manager_state_t state)
{
    switch (state)
    {
        case WIFI_MANAGER_STATE_UNINITIALIZED:
            return "UNINITIALIZED";

        case WIFI_MANAGER_STATE_READY:
            return "READY";

        case WIFI_MANAGER_STATE_CONNECTING:
            return "CONNECTING";

        case WIFI_MANAGER_STATE_WAITING_FOR_IP:
            return "WAITING_FOR_IP";

        case WIFI_MANAGER_STATE_CONNECTED:
            return "CONNECTED";

        case WIFI_MANAGER_STATE_DISCONNECTED:
            return "DISCONNECTED";

        case WIFI_MANAGER_STATE_FAILED:
            return "FAILED";

        default:
            return "UNKNOWN";
    }
}
