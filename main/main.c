/* Includes ----------------------------------------------------------------- */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

/* Common components ------------------------------------------------------- */
#include "app_common.h"
#include "board_config.h"

/* DISPLAY components ------------------------------------------------------- */
#include "display_driver.h"

/* LVGL Management components ----------------------------------------------- */
#include "ui_manager_lvgl.h"

/* SD Management components ------------------------------------------------- */
#include "sd_card_manager.h"

/* LVGL SD Management ------------------------------------------------------- */
#include "lvgl_sd_fs.h"

/* LVGL Image Handler------------------------------------------------------- */
#include "lvgl_image_handler.h"

/* Performance monitor ----------------------------------------------------- */
#include "performance_monitor.h"

/* Wifi manager ------------------------------------------------------------ */
#include "wifi_manager.h"

/* GUI Manager ------------------------------------------------------------- */
#include "app_gui.h"

/* Sensor manager ---------------------------------------------------------- */
// #include "sensor_DHT22.h"
#include "sensor_manager.h"

/* Macros ------------------------------------------------------------------- */
#define PERFORMANCE_MONITOR 0

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "MAIN_APP";

/* Static Variables --------------------------------------------------------- */
/* The UI manager borrows this handle for the lifetime of the application. */
static display_driver_handle_t display_handle;

/* DHT22 timing respects the sensor's minimum interval between reads. */
static const sensor_manager_config_t SENSOR_MANAGER_CONFIG =
{
    .sample_period_ms = 2500U,
    .stale_timeout_ms = 10000U,
};

/* Latest status copy used by the temporary main-loop diagnostic log. */
static sensor_manager_status_t s_sensor_data;

/* Function Prototypes ------------------------------------------------------ */
/** @brief Initialize NVS, ESP-NETIF, and the default event loop once. */
static esp_err_t network_platform_init(void);

/** @brief Convert and forward Wi-Fi manager events to the GUI queue. */
static void app_wifi_status_callback(
    const wifi_manager_status_t *status,
    void *user_data);

/** @brief Map a Wi-Fi manager state to its application GUI equivalent. */
static ui_wifi_state_t app_map_wifi_state(
    wifi_manager_state_t state);

/** @brief Map a sensor manager state to its application GUI equivalent. */
static ui_sensor_state_t app_map_sensor_state(
    sensor_manager_state_t state);

/** @brief Convert and forward sensor manager snapshots to the GUI queue. */
static void app_sensor_status_callback(
    const sensor_manager_status_t *status,
    void *user_context);

/* Application -------------------------------------------------------------- */
/** @brief Initialize the current application services and run diagnostics. */
void app_main(void)
{
    /* Project identity is emitted first to make firmware logs traceable. */
    ESP_LOGI(TAG, "PROJECT: %s", APP_PROJECT_NAME);
    ESP_LOGI(TAG, "VERSION: %s", APP_PROJECT_VER);
    ESP_LOGI(TAG, "BUILD DATE: %s", APP_PROJECT_VER_DATE);

#if PERFORMANCE_MONITOR
    esp_err_t monitor_ret =
        performance_monitor_start();

    if (monitor_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start performance monitor: %s",
            esp_err_to_name(monitor_ret)
        );
    }
#endif

    /*
     * Initialize shared network infrastructure before initializing
     * the Wi-Fi manager.
     */
    esp_err_t network_ret =
    network_platform_init();
    
    if (network_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize network platform: %s",
            esp_err_to_name(network_ret)
        );
        
        return;
    }

    // Display driver initialization
    esp_err_t ret = display_driver_init(&display_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display driver: %s", esp_err_to_name(ret));
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display driver: %s", esp_err_to_name(ret));
        return;
    }

    // Used to test the display by filling it with known colors. Uncomment to run the test.
    // ESP_ERROR_CHECK(display_driver_raw_color_test(&display_handle));

    // Initialize LVGL UI manager
    // Because LVGL core needs lvgl_init inside ui_manager_lvgl_init
    esp_err_t lvgl_ret = ui_manager_lvgl_init(&display_handle);


    // Initilize SD card manager
    esp_err_t sd_card_ret = sd_card_manager_init();
    if(sd_card_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Fail to initialize SD card driver");
        return;
    }
    else
    {
        // ESP_LOGI(TAG, "Start test SD card");
        // ESP_ERROR_CHECK(sd_card_manager_write_test_file());
        // ESP_ERROR_CHECK(sd_card_manager_read_test_file());
        // ESP_LOGI(TAG, "testing SD card is done");

        // Scan files inside specific folder
        // sd_card_manager_list_files(NULL);

        ESP_ERROR_CHECK(lvgl_sd_fs_register());
    }

    if (lvgl_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LVGL UI manager");
    }
    else
    {
        esp_err_t app_gui_ret = app_gui_init();
        if (app_gui_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "Failed to initialize application GUI: %s",
                     esp_err_to_name(app_gui_ret));
            return;
        }

        ESP_LOGI(TAG, "Start LVGL task handler");

        esp_err_t ui_task_ret = app_gui_start_ui_task();
        if (ui_task_ret != ESP_OK) {
            ESP_LOGE(TAG,
                        "Failed to start application GUI task: %s",
                        esp_err_to_name(ui_task_ret));
            return;
        }
        else
        {
            esp_err_t screen_ret =  app_gui_create_wifi_screen();

            if (screen_ret != ESP_OK) {
                ESP_LOGE(TAG,
                        "Failed to create Wi-Fi GUI: %s",
                        esp_err_to_name(screen_ret));
                return;
            }
            // lvgl_image_handler_example_task();
    
            // app_gui_start_running_demo_task();
        }

    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    esp_err_t wifi_ret =
        wifi_manager_init();

    if (wifi_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize Wi-Fi manager: %s",
            esp_err_to_name(wifi_ret)
        );

        return;
    }


esp_err_t callback_ret =
    wifi_manager_register_status_callback(
        app_wifi_status_callback,
        NULL
    );

if (callback_ret != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Failed to register Wi-Fi status callback: %s",
        esp_err_to_name(callback_ret)
    );

    return;
}
/*
 * Temporary hardcoded credentials for Sprint 2.
 *
 * Do not commit real credentials to a public repository.
 */
const wifi_manager_sta_config_t station_config = {
    .ssid = "HaiHeHuoc888",
    .password = "11233455",
};

esp_err_t connect_ret =
    wifi_manager_connect(&station_config);

if (connect_ret != ESP_OK) {
    ESP_LOGE(
        TAG,
        "Failed to start Wi-Fi connection: %s",
        esp_err_to_name(connect_ret)
    );

    return;
}

    // Scan and PrintOut Wifi SSID
    // const esp_err_t scan_ret =
    //         wifi_manager_scan_and_log();
    // dht22_sensor_data_t data;
    ESP_ERROR_CHECK(
    sensor_manager_init(
        &SENSOR_MANAGER_CONFIG));

    ESP_ERROR_CHECK(
    sensor_manager_register_callback(
        app_sensor_status_callback,
        NULL));

    ESP_ERROR_CHECK(
        sensor_manager_start());

    while (1)
    {
        sensor_manager_get_status(&s_sensor_data);
        ESP_LOGW(TAG, "Temp: %0.2f, Humid: %0.2f", 
            s_sensor_data.temperature_c, 
            s_sensor_data.humidity_percent
        );
        // ESP_LOGI(TAG, "Main loop running...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
}

/* Static Functions --------------------------------------------------------- */
static esp_err_t network_platform_init(void)
{
    /*
     * Initialize the default NVS partition.
     */
    esp_err_t ret = nvs_flash_init();
    

    /*
     * These errors can occur when:
     *
     * - the NVS partition has no free pages;
     * - the stored NVS format belongs to another ESP-IDF version.
     *
     * Erasing is acceptable during the current development phase because
     * Sprint 5 NVS configuration storage has not been implemented yet.
     */
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {

        ESP_LOGW(
            TAG,
            "NVS partition requires erase: %s",
            esp_err_to_name(ret)
        );

        ret = nvs_flash_erase();

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to erase NVS: %s",
                esp_err_to_name(ret)
            );

            return ret;
        }

        ret = nvs_flash_init();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize NVS: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * Initialize ESP-NETIF and the underlying TCP/IP stack.
     *
     * This must be called once before creating Wi-Fi or Ethernet
     * network interfaces.
     */
    ret = esp_netif_init();

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize ESP-NETIF: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }
    /*
     * Create the default system event loop.
     *
     * Wi-Fi and IP events will later be delivered through this loop.
     */
    ret = esp_event_loop_create_default();

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create default event loop: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    ESP_LOGI(TAG, "Network platform initialized");

    return ESP_OK;
}


static ui_wifi_state_t app_map_wifi_state(
    wifi_manager_state_t state)
{
    switch (state)
    {
        case WIFI_MANAGER_STATE_CONNECTING:
            return UI_WIFI_STATE_CONNECTING;

        case WIFI_MANAGER_STATE_WAITING_FOR_IP:
            return UI_WIFI_STATE_WAITING_FOR_IP;

        case WIFI_MANAGER_STATE_CONNECTED:
            return UI_WIFI_STATE_CONNECTED;

        case WIFI_MANAGER_STATE_DISCONNECTED:
            return UI_WIFI_STATE_DISCONNECTED;

        case WIFI_MANAGER_STATE_FAILED:
            return UI_WIFI_STATE_FAILED;

        default:
            return UI_WIFI_STATE_IDLE;
    }
}

static void app_wifi_status_callback(
    const wifi_manager_status_t *status,
    void *user_data)
{
    (void)user_data;

    if (status == NULL) {
        return;
    }

    ui_wifi_status_t ui_status = {
        .state =
            app_map_wifi_state(status->state),

        .rssi_dbm =
            status->rssi_dbm,

        .disconnect_reason =
            status->disconnect_reason,

        .has_ipv4_address =
            status->has_ipv4_address,

        .rssi_valid =
            status->rssi_valid,
    };

    snprintf(
        ui_status.ssid,
        sizeof(ui_status.ssid),
        "%s",
        status->ssid
    );

    snprintf(
        ui_status.ipv4_address,
        sizeof(ui_status.ipv4_address),
        "%s",
        status->ipv4_address
    );

    const esp_err_t ret =
        app_gui_post_wifi_status(
            &ui_status
        );

    if (ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Failed to forward Wi-Fi status to UI: %s",
            esp_err_to_name(ret)
        );
    }


    ESP_LOGI(TAG, "*------------------------------------------------------------------------------------------*");
    ESP_LOGI(
        TAG,
        "Wi-Fi callback: state=%s, ssid=%s, ip=%s, reason=%u",
        wifi_manager_state_to_string(status->state),
        status->ssid[0] != '\0'
            ? status->ssid
            : "<none>",
        status->has_ipv4_address
            ? status->ipv4_address
            : "<none>",
        (unsigned int)status->disconnect_reason
    );
    ESP_LOGI(TAG, "*------------------------------------------------------------------------------------------*");
}

static ui_sensor_state_t app_map_sensor_state(
    sensor_manager_state_t state)
{
    switch (state)
    {
        case SENSOR_MANAGER_STATE_READY:
            return UI_SENSOR_STATE_READY;

        case SENSOR_MANAGER_STATE_DEGRADED:
            return UI_SENSOR_STATE_DEGRADED;

        case SENSOR_MANAGER_STATE_ERROR:
            return UI_SENSOR_STATE_ERROR;

        case SENSOR_MANAGER_STATE_UNINITIALIZED:
        case SENSOR_MANAGER_STATE_INITIALIZED:
        case SENSOR_MANAGER_STATE_RUNNING:
        default:
            return UI_SENSOR_STATE_INITIALIZING;
    }
}

static void app_sensor_status_callback(
    const sensor_manager_status_t *status,
    void *user_context)
{
    (void)user_context;

    if (status == NULL)
    {
        return;
    }

    ui_sensor_status_t ui_status =
    {
        .state =
            app_map_sensor_state(status->state),

        .temperature_c =
            status->temperature_c,

        .humidity_percent =
            status->humidity_percent,

        .data_valid =
            status->data_valid,

        .data_stale =
            status->data_stale,

        .last_error =
            status->last_error,
    };

    esp_err_t error =
        app_gui_post_sensor_status(&ui_status);

    if (error != ESP_OK)
    {
        ESP_LOGD(
            TAG,
            "Sensor GUI update dropped: %s",
            esp_err_to_name(error));
    }
}
