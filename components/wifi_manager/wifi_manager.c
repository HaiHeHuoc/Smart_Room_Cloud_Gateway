#include "wifi_manager.h"

#include <string.h>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

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

    /*
     * ESP-NETIF object representing the Wi-Fi Station interface.
     */
    esp_netif_t *station_netif;

    /*
     * Handles returned when registering event callbacks.
     * These handles can later be used to unregister the callbacks.
     */
    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;

    wifi_manager_status_t status;

    wifi_manager_status_callback_t status_callback;

    void *status_callback_user_data;
} wifi_manager_context_t;

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static wifi_manager_context_t s_wifi_manager = {
    .initialized = false,
    .station_netif = NULL,

    .wifi_event_instance = NULL,
    .ip_event_instance = NULL,

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
static void wifi_manager_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
);

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */
static void wifi_manager_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    /*
     * These parameters will be used in later steps.
     */
    (void)handler_argument;
    (void)event_data;


    if (event_base == WIFI_EVENT) {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "Event: WIFI_EVENT_STA_START");
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Event: WIFI_EVENT_STA_CONNECTED");
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "Event: WIFI_EVENT_STA_DISCONNECTED");
                break;

            case WIFI_EVENT_STA_STOP:
                ESP_LOGI(TAG, "Event: WIFI_EVENT_STA_STOP");
                break;

            default:
                ESP_LOGD(
                    TAG,
                    "Unhandled WIFI_EVENT id=%ld",
                    (long)event_id
                );
                break;
        }

        return;
    }

    if (event_base == IP_EVENT) {
        switch (event_id)
        {
            case IP_EVENT_STA_GOT_IP:
                ESP_LOGI(TAG, "Event: IP_EVENT_STA_GOT_IP");
                break;

            case IP_EVENT_STA_LOST_IP:
                ESP_LOGI(TAG, "Event: IP_EVENT_STA_LOST_IP");
                break;

            default:
                ESP_LOGD(
                    TAG,
                    "Unhandled IP_EVENT id=%ld",
                    (long)event_id
                );
                break;
        }
    }
}

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */

esp_err_t wifi_manager_init(void)
{
    if(s_wifi_manager.initialized == true)
    {
        ESP_LOGW(TAG, "Wi-Fi manager is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Create the default Wi-Fi Station network interface.
     *
     * Prerequisites:
     *
     *     esp_netif_init()
     *     esp_event_loop_create_default()
     *
     * Both have already been initialized by network_platform_init().
     */
    esp_netif_t *station_netif =
        esp_netif_create_default_wifi_sta();

    if (station_netif == NULL) {
        ESP_LOGE(
            TAG,
            "Failed to create default Wi-Fi Station interface"
        );

        return ESP_FAIL;
    }

    /*
     * Initialize the Wi-Fi driver using ESP-IDF's recommended
     * default configuration.
     */
    wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();

    esp_err_t ret =
        esp_wifi_init(&wifi_init_config);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize Wi-Fi driver: %s",
            esp_err_to_name(ret)
        );

        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }


    /*
     * During Sprint 2, credentials are temporary and hardcoded.
     *
     * Do not let the Wi-Fi driver silently persist them into NVS.
     * Persistent configuration will be owned by config_manager
     * during Sprint 5.
     */
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure Wi-Fi storage: %s",
            esp_err_to_name(ret)
        );

        const esp_err_t deinit_ret =
            esp_wifi_deinit();

        if (deinit_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Wi-Fi cleanup failed: %s",
                esp_err_to_name(deinit_ret)
            );
        }

        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }


    /*
     * Configure the device as a Wi-Fi client.
     *
     * Station mode:
     *
     *     ESP32-S3 → connects to an existing router/access point
     */
    ret = esp_wifi_set_mode(WIFI_MODE_STA);


    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set Wi-Fi Station mode: %s",
            esp_err_to_name(ret)
        );

        const esp_err_t deinit_ret =
            esp_wifi_deinit();

        if (deinit_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Wi-Fi cleanup failed: %s",
                esp_err_to_name(deinit_ret)
            );
        }

        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }

    ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_manager_event_handler,
        NULL,
        &s_wifi_manager.wifi_event_instance
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register Wi-Fi event handler: %s",
            esp_err_to_name(ret)
        );

        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }

    ret = esp_event_handler_instance_register(
        IP_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_manager_event_handler,
        NULL,
        &s_wifi_manager.ip_event_instance
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register IP event handler: %s",
            esp_err_to_name(ret)
        );

        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_manager.wifi_event_instance
        );

        s_wifi_manager.wifi_event_instance = NULL;

        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }
    
    ret = esp_wifi_start();
    
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start Wi-Fi Station: %s",
            esp_err_to_name(ret)
        );
        
        /*
        * Undo IP event registration.
        */
       esp_event_handler_instance_unregister(
           IP_EVENT,
           ESP_EVENT_ANY_ID,
           s_wifi_manager.ip_event_instance
        );
        
        s_wifi_manager.ip_event_instance = NULL;
        
        /*
        * Undo Wi-Fi event registration.
        */
       esp_event_handler_instance_unregister(
           WIFI_EVENT,
           ESP_EVENT_ANY_ID,
           s_wifi_manager.wifi_event_instance
        );
        
        s_wifi_manager.wifi_event_instance = NULL;
        
        /*
        * Release Wi-Fi driver and Station interface.
        */
       const esp_err_t deinit_ret =
       esp_wifi_deinit();
       
       if (deinit_ret != ESP_OK) {
           ESP_LOGW(
               TAG,
               "Failed to deinitialize Wi-Fi after start error: %s",
               esp_err_to_name(deinit_ret)
            );
        }

        esp_netif_destroy_default_wifi(station_netif);
        
        return ret;
    }
    
        /*
         * Commit component state only after all initialization steps succeed.
         */
        s_wifi_manager.station_netif =
            station_netif;
    
        s_wifi_manager.initialized =
            true;
    
        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_READY;
    
        ESP_LOGI(
            TAG,
            "Wi-Fi manager initialized: mode=%s, storage=%s",
            "STATION",
            "RAM"
        );
    
    return ESP_OK;
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
