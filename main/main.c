/* Includes ----------------------------------------------------------------- */
#include <stdint.h>
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
#include "sensor_manager.h"

/* cloud_manager ----------------------------------------------------------- */
#include "cloud_manager.h"

/* firebase auth ----------------------------------------------------------- */
#include "firebase_auth.h"

/* Config manager ---------------------------------------------------------- */
#include "config_manager.h"

/* app network coordinator ------------------------------------------------- */
#include "app_network_coordinator.h"

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
    .sample_period_ms = 2000U,
    .stale_timeout_ms = 10000U,
};

static const app_network_coordinator_config_t
    APP_NETWORK_COORDINATOR_CONFIG =
{
    .provisioning_timeout_ms =
        120000U,

    .provisioning_poll_period_ms =
        200U,
};

// Cloud configuration
static const cloud_manager_config_t CLOUD_MANAGER_CONFIG =
{
    .firebase_latest_url =
        "https://esp32-smart-room-gateway-default-rtdb."
        "asia-southeast1.firebasedatabase.app/"
        "devices/esp32s3-001/latest.json",

    .publish_period_ms = 10000U,
};

static const firebase_auth_config_t FIREBASE_AUTH_CONFIG =
{
    .api_key = FIREBASE_API_KEY,
    .email = FIREBASE_DEVICE_EMAIL,
    .password = FIREBASE_DEVICE_PASSWORD,
    .expected_uid = FIREBASE_DEVICE_UID,
    .refresh_margin_seconds = 300U,
};

/* Function Prototypes ------------------------------------------------------ */
/**
 * @brief Initialize NVS, config storage, ESP-NETIF, and the event loop.
 *
 * @param[out] wifi_config_state Resolved boot-time Wi-Fi configuration state.
 */
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

/** @brief Map a cloud manager state to its application GUI equivalent. */
static ui_cloud_state_t app_map_cloud_state(
    cloud_manager_state_t state);

/** @brief Convert and forward cloud manager snapshots to the GUI queue. */
static void app_cloud_status_callback(
    const cloud_manager_status_t *status,
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

        esp_err_t fs_ret = lvgl_sd_fs_register();
        if (fs_ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "Failed to register LVGL SD filesystem: %s",
                esp_err_to_name(fs_ret));
            return;
        }
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

        ESP_LOGD(TAG, "Starting LVGL task handler");

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

    /*
     * Register before starting network orchestration so the GUI observes the
     * first stored-connection or provisioning status transition.
     */
    esp_err_t callback_ret =
        wifi_manager_register_status_callback(
            app_wifi_status_callback,
            NULL);

    if (callback_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register Wi-Fi status callback: %s",
            esp_err_to_name(callback_ret));

        return;
    }

    esp_err_t coordinator_ret =
        app_network_coordinator_init(
            &APP_NETWORK_COORDINATOR_CONFIG);

    if (coordinator_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize network coordinator: %s",
            esp_err_to_name(coordinator_ret));

        return;
    }

    coordinator_ret =
        app_network_coordinator_start();

    if (coordinator_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start network coordinator: %s",
            esp_err_to_name(coordinator_ret));

        return;
    }


    esp_err_t service_ret =
        sensor_manager_init(
            &SENSOR_MANAGER_CONFIG);

    if (service_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize sensor manager: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret =
        sensor_manager_register_callback(
            app_sensor_status_callback,
            NULL);

    if (service_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register sensor callback: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret = sensor_manager_start();

    if (service_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start sensor manager: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret =
        firebase_auth_init(
            &FIREBASE_AUTH_CONFIG);

    if (service_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize Firebase Authentication: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret =
        cloud_manager_init(
            &CLOUD_MANAGER_CONFIG);

    if (service_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to initialize cloud manager: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret =
        cloud_manager_register_status_callback(
            app_cloud_status_callback,
            NULL);

    if (service_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register cloud status callback: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret = cloud_manager_start();

    if (service_ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to start cloud manager: %s",
            esp_err_to_name(service_ret));
        return;
    }

    while (1)
    {
        // ESP_LOGI(TAG, "Main loop running...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
}
/* Static Functions --------------------------------------------------------- */
static esp_err_t network_platform_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize NVS: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ret = config_manager_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize config manager: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ret = esp_netif_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize ESP-NETIF: %s",
            esp_err_to_name(ret));

        return ret;
    }
    /*
     * Create the default system event loop.
     *
     * Wi-Fi and IP events will later be delivered through this loop.
     */
    ret = esp_event_loop_create_default();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to create default event loop: %s",
            esp_err_to_name(ret));

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

        case WIFI_MANAGER_STATE_RETRY_WAIT:
            return UI_WIFI_STATE_RETRY_WAIT;

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


    ESP_LOGD(TAG, "*------------------------------------------------------------------------------------------*");
    ESP_LOGD(
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
    ESP_LOGD(TAG, "*------------------------------------------------------------------------------------------*");
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

    const cloud_sensor_telemetry_t telemetry =
    {
        .temperature_c =
            status->temperature_c,

        .humidity_percent =
            status->humidity_percent,

        .data_valid =
            status->data_valid,

        .data_stale =
            status->data_stale,

        .sensor_state =
            (int32_t)status->state,

        .last_error =
            status->last_error,

        .sample_uptime_ms =
            status->last_success_time_ms,
    };

    error =
        cloud_manager_post_sensor_telemetry(
            &telemetry);

    if (error != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Sensor cloud update dropped: %s",
            esp_err_to_name(error));
    }
}

static ui_cloud_state_t app_map_cloud_state(
    cloud_manager_state_t state)
{
    switch (state)
    {
        case CLOUD_MANAGER_STATE_INITIALIZED:
        case CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK:
        case CLOUD_MANAGER_STATE_WAITING_FOR_DATA:
            return UI_CLOUD_STATE_WAITING;

        case CLOUD_MANAGER_STATE_UPLOADING:
            return UI_CLOUD_STATE_UPLOADING;

        case CLOUD_MANAGER_STATE_ONLINE:
            return UI_CLOUD_STATE_ONLINE;

        case CLOUD_MANAGER_STATE_RETRY_WAIT:
            return UI_CLOUD_STATE_RETRY_WAIT;

        case CLOUD_MANAGER_STATE_AUTH_ERROR:
            return UI_CLOUD_STATE_AUTH_ERROR;

        case CLOUD_MANAGER_STATE_ERROR:
            return UI_CLOUD_STATE_ERROR;

        case CLOUD_MANAGER_STATE_UNINITIALIZED:
        default:
            return UI_CLOUD_STATE_UNKNOWN;
    }
}

static void app_cloud_status_callback(
    const cloud_manager_status_t *status,
    void *user_context)
{
    (void)user_context;

    if (status == NULL)
    {
        return;
    }

    const ui_cloud_status_t ui_status =
    {
        .state =
            app_map_cloud_state(status->state),

        .last_error =
            status->last_error,

        .last_http_status =
            status->last_http_status,
    };

    const esp_err_t error =
        app_gui_post_cloud_status(&ui_status);

    if (error != ESP_OK)
    {
        ESP_LOGD(
            TAG,
            "Cloud GUI update dropped: %s",
            esp_err_to_name(error));
    }
}


/* Static Functions --------------------------------------------------------- */
