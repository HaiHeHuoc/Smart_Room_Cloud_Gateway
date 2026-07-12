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

/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */
#define PERFORMANCE_MONITOR 1

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
static const char *TAG = "MAIN_APP";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static display_driver_handle_t display_handle;

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */
static esp_err_t network_platform_init(void);

// For wifi section
static void wifi_status_test_task(void *argument);
static void wifi_disconnect_test_task(void *argument);
static void app_wifi_status_callback(
    const wifi_manager_status_t *status,
    void *user_data);
static ui_wifi_state_t app_map_wifi_state(
    wifi_manager_state_t state);

/* Application ------------------------------------------------------------- */
void app_main(void)
{
    // Project information
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
        ESP_LOGI(TAG, "Start LVGL task handler");

        esp_err_t ui_task_ret = ui_manager_lvgl_start_UI_task();
        if (ui_task_ret != ESP_OK) {
            ESP_LOGE(TAG,
                        "Failed to start LVGL UI task: %s",
                        esp_err_to_name(ui_task_ret));
            return;
        }

        // lvgl_image_handler_example_task();

        app_gui_start_running_demo_task();
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

    // xTaskCreate(
    //     wifi_status_test_task,
    //     "wifi_status_test",
    //     3072U,
    //     NULL,
    //     3U,
    //     NULL
    // );

    xTaskCreate(
        wifi_disconnect_test_task,
        "wifi_disconnect_test",
        3072U,
        NULL,
        3U,
        NULL
    );

    while (1)
    {
        // ESP_LOGI(TAG, "Main loop running...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    
}

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

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


static void wifi_status_test_task(void *argument)
{
    (void)argument;

    vTaskDelay(pdMS_TO_TICKS(5000));

    wifi_manager_status_t status = {0};

    const esp_err_t ret =
        wifi_manager_get_status(&status);

    if (ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Wi-Fi status: state=%s, ssid=%s, ip=%s",
            wifi_manager_state_to_string(status.state),
            status.ssid,
            status.has_ipv4_address
                ? status.ipv4_address
                : "<none>"
        );
    }
    else {
        ESP_LOGE(
            TAG,
            "Failed to get Wi-Fi status: %s",
            esp_err_to_name(ret)
        );
    }

    vTaskDelete(NULL);
}

static void wifi_disconnect_test_task(void *argument)
{
    (void)argument;

    while (true) {
        wifi_manager_status_t status = {0};

        const esp_err_t ret =
            wifi_manager_get_status(&status);

        if (ret == ESP_OK) {
            ESP_LOGI(
                TAG,
                "Wi-Fi: state=%s, ip=%s, reason=%u",
                wifi_manager_state_to_string(
                    status.state
                ),
                status.has_ipv4_address
                    ? status.ipv4_address
                    : "<none>",
                (unsigned int)status.disconnect_reason
            );
        }

        if (wifi_manager_is_connected()) {
            int8_t rssi_dbm = 0;

            const esp_err_t ret =
                wifi_manager_get_rssi(&rssi_dbm);
            ESP_LOGI(TAG, "*------------------------------------------------------------------------------------------*");
            if (ret == ESP_OK) {
                ESP_LOGI(
                    TAG,
                    "Connected Wi-Fi RSSI: %d dBm",
                    (int)rssi_dbm
                );
            }
            else {
                ESP_LOGE(
                    TAG,
                    "Failed to read RSSI: %s",
                    esp_err_to_name(ret)
                );
            }
            ESP_LOGI(TAG, "*------------------------------------------------------------------------------------------*");
        }

        vTaskDelay(pdMS_TO_TICKS(20000));
    }
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
        ui_manager_lvgl_post_wifi_status(
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

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
