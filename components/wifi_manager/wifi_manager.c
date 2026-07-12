#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>

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

/*
 * Protect s_wifi_manager.status because:
 *
 * - ESP event-loop task writes the status;
 * - application/UI tasks read the status.
 */
static portMUX_TYPE s_status_lock =
    portMUX_INITIALIZER_UNLOCKED;


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
                taskENTER_CRITICAL(&s_status_lock);

                    s_wifi_manager.status.state =
                    WIFI_MANAGER_STATE_WAITING_FOR_IP;
                    
                    s_wifi_manager.status.disconnect_reason =
                    0U;
                    
                    s_wifi_manager.status.has_ipv4_address =
                    false;
                    
                    s_wifi_manager.status.ipv4_address[0] =
                    '\0';

                taskEXIT_CRITICAL(&s_status_lock);

                ESP_LOGI(TAG, "Event: WIFI_EVENT_STA_CONNECTED", "waiting for IPv4 address");
                break;

            case WIFI_EVENT_STA_DISCONNECTED:

                const wifi_event_sta_disconnected_t *event =
                    (const wifi_event_sta_disconnected_t *)event_data;

                uint16_t disconnect_reason = (uint16_t)WIFI_REASON_UNSPECIFIED;

                if (event != NULL) {
                    disconnect_reason =
                        (uint16_t)event->reason;
                }
                else {
                    ESP_LOGE(
                        TAG,
                        "WIFI_EVENT_STA_DISCONNECTED has no event data"
                    );
                }

                taskENTER_CRITICAL(&s_status_lock);

                s_wifi_manager.status.state =
                    WIFI_MANAGER_STATE_DISCONNECTED;

                s_wifi_manager.status.disconnect_reason =
                    disconnect_reason;

                s_wifi_manager.status.has_ipv4_address =
                    false;

                s_wifi_manager.status.ipv4_address[0] =
                    '\0';

                s_wifi_manager.status.rssi_valid =
                    false;

                s_wifi_manager.status.rssi_dbm =
                    0;

                taskEXIT_CRITICAL(&s_status_lock);

                ESP_LOGI(TAG, "Event: WIFI_EVENT_STA_DISCONNECTED");

                /*
                * Chưa reconnect tự động tại đây.
                * Reconnect/backoff thuộc Sprint 8.
                */

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
                const ip_event_got_ip_t *got_ip_event =
                    (const ip_event_got_ip_t *)event_data;

                if (got_ip_event == NULL) {
                    ESP_LOGE(
                        TAG,
                        "IP_EVENT_STA_GOT_IP contains no event data"
                    );

                    break;
                }

                /*
                * Format into a local buffer before entering the critical section.
                * snprintf() should not run while interrupts/scheduling are restricted.
                */
                char ipv4_address[WIFI_MANAGER_IPV4_STRING_SIZE] = {0};

                const int written = snprintf(
                    ipv4_address,
                    sizeof(ipv4_address),
                    IPSTR,
                    IP2STR(&got_ip_event->ip_info.ip)
                );

                if ((written <= 0) ||
                    ((size_t)written >= sizeof(ipv4_address))) {

                    ESP_LOGE(
                        TAG,
                        "Failed to format Station IPv4 address"
                    );

                    break;
                }

                taskENTER_CRITICAL(&s_status_lock);

                memcpy(
                    s_wifi_manager.status.ipv4_address,
                    ipv4_address,
                    sizeof(s_wifi_manager.status.ipv4_address)
                );

                s_wifi_manager.status.has_ipv4_address =
                    true;

                s_wifi_manager.status.state =
                    WIFI_MANAGER_STATE_CONNECTED;

                s_wifi_manager.status.disconnect_reason =
                    0U;

                taskEXIT_CRITICAL(&s_status_lock);

                ESP_LOGI(
                    TAG,
                    "Event: IP_EVENT_STA_GOT_IP, address=%s",
                    ipv4_address
                );

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

static const char *wifi_manager_rssi_to_quality(int8_t rssi)
{
    if (rssi >= -50) {
        return "EXCELLENT";
    }

    if (rssi >= -60) {
        return "GOOD";
    }

    if (rssi >= -70) {
        return "FAIR";
    }

    return "WEAK";
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

    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    /*
     * Avoid starting another connection while one is already active.
     */
    if ((s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_CONNECTING) ||
        (s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_WAITING_FOR_IP) ||
        (s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_CONNECTED)) {

        ESP_LOGW(
            TAG,
            "Wi-Fi connection is already active: state=%s",
            wifi_manager_state_to_string(
                s_wifi_manager.status.state
            )
        );

        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_FALSE(config->ssid != NULL ||
        config->ssid[0] == '\0', 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Wifi SSID is NULL"
    );

    ESP_RETURN_ON_FALSE(config->password != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Wifi password is NULL"
    );


    const size_t ssid_length =
        strlen(config->ssid);

    const size_t password_length =
        strlen(config->password);


    /*
     * wifi_config_t.sta.ssid has space for 32 bytes.
     * An SSID can legally use all 32 bytes.
     */
    if (ssid_length >
        WIFI_MANAGER_SSID_MAX_LENGTH) {

        ESP_LOGE(
            TAG,
            "SSID is too long: %u bytes, maximum=%u",
            (unsigned int)ssid_length,
            (unsigned int)WIFI_MANAGER_SSID_MAX_LENGTH
        );

        return ESP_ERR_INVALID_ARG;
    }

    if (password_length >
        WIFI_MANAGER_PASSWORD_MAX_LENGTH) {

        ESP_LOGE(
            TAG,
            "Password is too long: %u bytes, maximum=%u",
            (unsigned int)password_length,
            (unsigned int)WIFI_MANAGER_PASSWORD_MAX_LENGTH
        );

        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 3. Build ESP-IDF Station configuration.
     *
     * Zero-initialization gives sensible defaults:
     *
     * - scan all necessary channels;
     * - do not lock to a specific BSSID;
     * - use default RSSI threshold.
     */
    wifi_config_t wifi_config = {0};

    memcpy(
        wifi_config.sta.ssid,
        config->ssid,
        ssid_length
    );

    if (password_length > 0U) {
        memcpy(
            wifi_config.sta.password,
            config->password,
            password_length
        );

        /*
         * Reject deprecated/insecure APs below WPA2.
         * WPA3 is still accepted because it is stronger than WPA2.
         */
        wifi_config.sta.threshold.authmode =
            WIFI_AUTH_WPA2_PSK;
    }
    else {
        /*
         * Empty password means an open access point.
         */
        wifi_config.sta.threshold.authmode =
            WIFI_AUTH_OPEN;
    }

    /*
     * 4. Apply Station configuration to the Wi-Fi driver.
     */
    esp_err_t ret =
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to set Wi-Fi Station config: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * 5. Prepare internal status before calling esp_wifi_connect().
     *
     * Setting CONNECTING first avoids a race where an asynchronous
     * Wi-Fi event arrives before the state is updated.
     */
    memset(
        s_wifi_manager.status.ssid,
        0,
        sizeof(s_wifi_manager.status.ssid)
    );

    memcpy(
        s_wifi_manager.status.ssid,
        config->ssid,
        ssid_length
    );


    s_wifi_manager.status.ssid[ssid_length] =
        '\0';

    s_wifi_manager.status.ipv4_address[0] =
        '\0';

    s_wifi_manager.status.has_ipv4_address =
        false;

    s_wifi_manager.status.rssi_valid =
        false;

    s_wifi_manager.status.disconnect_reason =
        0U;

    s_wifi_manager.status.state =
        WIFI_MANAGER_STATE_CONNECTING;

    ESP_LOGI(
        TAG,
        "Connecting to Wi-Fi SSID: %s",
        s_wifi_manager.status.ssid
    );

    /*
     * 6. Start the asynchronous connection process.
     */
    ret = esp_wifi_connect();

    if (ret != ESP_OK) {
        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_FAILED;

        ESP_LOGE(
            TAG,
            "Failed to start Wi-Fi connection: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    return ESP_OK;
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

    taskENTER_CRITICAL(&s_status_lock);

    memcpy(
        status,
        &s_wifi_manager.status,
        sizeof(*status)
    );

    taskEXIT_CRITICAL(&s_status_lock);

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

    /*
     * Check state quickly under the status lock.
     */
    bool connected = false;

    taskENTER_CRITICAL(&s_status_lock);

    connected =
        (s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_CONNECTED) &&
        s_wifi_manager.status.has_ipv4_address;

    taskEXIT_CRITICAL(&s_status_lock);

    if (!connected) {
        ESP_LOGW(TAG, "Cannot read RSSI while Wi-Fi is disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Do not call ESP Wi-Fi APIs inside a critical section.
     */
    int current_rssi = 0;

    const esp_err_t ret =
        esp_wifi_sta_get_rssi(&current_rssi);

    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);

        s_wifi_manager.status.rssi_valid = false;
        s_wifi_manager.status.rssi_dbm = 0;

        taskEXIT_CRITICAL(&s_status_lock);

        ESP_LOGE(
            TAG,
            "Failed to read Wi-Fi RSSI: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    const int8_t rssi_value =
        (int8_t)current_rssi;

    taskENTER_CRITICAL(&s_status_lock);

    s_wifi_manager.status.rssi_dbm =
        rssi_value;

    s_wifi_manager.status.rssi_valid =
        true;

    taskEXIT_CRITICAL(&s_status_lock);

    *rssi_dbm = rssi_value;

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{

    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    bool connected = false;

    taskENTER_CRITICAL(&s_status_lock);

    connected =
        (s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_CONNECTED) &&
        s_wifi_manager.status.has_ipv4_address;

    taskEXIT_CRITICAL(&s_status_lock);

    return connected;
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

esp_err_t wifi_manager_scan_and_log(void)
{
    if (!s_wifi_manager.initialized) {
        ESP_LOGE(
            TAG,
            "Cannot scan because Wi-Fi manager is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Scan configuration:
     *
     * ssid        = NULL : do not filter by SSID
     * bssid       = NULL : do not filter by BSSID
     * channel     = 0    : scan all supported channels
     * show_hidden = true : include APs with hidden SSIDs
     * scan_type   = active scan
     */
    const wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0U,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,

        .scan_time = {
            .active = {
                .min = 0U,
                .max = 120U,
            },
        },

        /*
         * While connected, periodically return to the current AP's
         * channel so normal Wi-Fi traffic still has an opportunity
         * to run.
         */
        .home_chan_dwell_time = 30U,
    };

    ESP_LOGI(TAG, "Starting all-channel Wi-Fi scan");

    /*
     * block = true:
     *
     * This task waits here until the scan finishes.
     *
     * A blocked scan does not generate WIFI_EVENT_SCAN_DONE.
     */
    esp_err_t ret =
        esp_wifi_scan_start(
            &scan_config,
            true
        );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start Wi-Fi scan: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    uint16_t ap_count = 0U;

    ret = esp_wifi_scan_get_ap_num(&ap_count);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to obtain scanned AP count: %s",
            esp_err_to_name(ret)
        );

        /*
         * Release result memory owned by the Wi-Fi driver.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to clear Wi-Fi scan list: %s",
                esp_err_to_name(clear_ret)
            );
        }

        return ret;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi scan completed: found=%u AP records",
        (unsigned int)ap_count
    );

    if (ap_count == 0U) {
        /*
         * No records will be fetched, so explicitly clear the list.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to clear empty Wi-Fi scan list: %s",
                esp_err_to_name(clear_ret)
            );

            return clear_ret;
        }

        return ESP_OK;
    }

    wifi_ap_record_t *ap_records =
        calloc(
            ap_count,
            sizeof(*ap_records)
        );

    if (ap_records == NULL) {
        ESP_LOGE(
            TAG,
            "No memory for %u Wi-Fi AP records",
            (unsigned int)ap_count
        );

        /*
         * The scan-result list is still owned by the Wi-Fi driver.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to clear scan list after allocation error: %s",
                esp_err_to_name(clear_ret)
            );
        }

        return ESP_ERR_NO_MEM;
    }

    /*
     * Input:
     *     records_to_read is the capacity of ap_records.
     *
     * Output:
     *     records_to_read becomes the number of records returned.
     */
    uint16_t records_to_read = ap_count;

    ret = esp_wifi_scan_get_ap_records(
        &records_to_read,
        ap_records
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to retrieve Wi-Fi AP records: %s",
            esp_err_to_name(ret)
        );

        /*
         * Be defensive in case the driver still owns scan entries.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Failed to clear scan list after retrieval error: %s",
                esp_err_to_name(clear_ret)
            );
        }

        free(ap_records);

        return ret;
    }

    ESP_LOGI(
        TAG,
        "------------------------------------------------------------"
    );

    ESP_LOGI(
        TAG,
        " No. | RSSI | Channel | Quality   | SSID"
    );

    ESP_LOGI(
        TAG,
        "------------------------------------------------------------"
    );

    for (uint16_t index = 0U;
         index < records_to_read;
         ++index) {

        const wifi_ap_record_t *record =
            &ap_records[index];

        const char *ssid =
            record->ssid[0] != '\0'
                ? (const char *)record->ssid
                : "<hidden>";

        ESP_LOGI(
            TAG,
            "%4u | %4d | %7u | %-9s | %s",
            (unsigned int)(index + 1U),
            (int)record->rssi,
            (unsigned int)record->primary,
            wifi_manager_rssi_to_quality(record->rssi),
            ssid
        );
    }

    ESP_LOGI(
        TAG,
        "------------------------------------------------------------"
    );

    free(ap_records);

    return ESP_OK;
}