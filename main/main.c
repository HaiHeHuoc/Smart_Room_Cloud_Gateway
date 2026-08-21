/* Includes ----------------------------------------------------------------- */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "sdkconfig.h"

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

/* Performance monitor ----------------------------------------------------- */
#include "performance_monitor.h"

/* Time manager ------------------------------------------------------------ */
#include "time_manager.h"

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

/* app reset coordinator --------------------------------------------------- */
#include "app_reset_coordinator.h"

/* Button manager ----------------------------------------------------------- */
#include "button_manager.h"

/* Audio manager ------------------------------------------------------------ */
#include "audio_manager.h"
#if CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST
#include "audio_api_test_task.h"
#endif

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

static const button_manager_config_t BUTTON_MANAGER_CONFIG =
{
    .gpio_num =
        FACTORY_RESET_BUTTON_GPIO,

    .active_level =
        FACTORY_RESET_BUTTON_ACTIVE_LEVEL,

    .poll_period_ms =
        FACTORY_RESET_BUTTON_POLL_PERIOD_MS,

    .debounce_ms =
        FACTORY_RESET_BUTTON_DEBOUNCE_MS,

    .long_press_ms =
        FACTORY_RESET_BUTTON_LONG_PRESS_MS,
};

/* Coordinator owns provisioning lifecycle; Wi-Fi reconnect remains separate. */
static const app_network_coordinator_config_t
    APP_NETWORK_COORDINATOR_CONFIG =
{
    .provisioning_timeout_ms =
        120000U,

    .provisioning_connection_grace_ms =
        30000U,

    .provisioning_poll_period_ms =
        200U,

    .provisioning_max_sessions =
        3U,

    .provisioning_retry_backoff_ms =
        1500U,

    .provisioning_failure_dwell_ms =
        1000U,
};

/* Cloud state and its telemetry queue are initialized before sensor start. */
static const cloud_manager_config_t CLOUD_MANAGER_CONFIG =
{
    .firebase_latest_url =
        "https://esp32-smart-room-gateway-default-rtdb."
        "asia-southeast1.firebasedatabase.app/"
        "devices/esp32s3-001/latest.json",

    .publish_period_ms = 60000U,
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
 */
static esp_err_t network_platform_init(void);

/**
 * @brief Forward one button-manager event to the reset coordinator queue.
 *
 * Runs in button task context and must not directly access storage or LVGL.
 */
static void app_button_event_callback(
    const button_manager_event_data_t *event_data,
    void *user_context);

/**
 * @brief Fan out Wi-Fi state to coordinator, GUI, cloud, and time services.
 */
static void app_wifi_status_callback(
    const wifi_manager_status_t *status,
    void *user_data);

/** @brief Map a Wi-Fi manager state to its application GUI equivalent. */
static ui_wifi_state_t app_map_wifi_state(
    wifi_manager_state_t state);

/** @brief Map a sensor manager state to its application GUI equivalent. */
static ui_sensor_state_t app_map_sensor_state(
    sensor_manager_state_t state);

/** @brief Copy a coherent time-manager view into cloud-owned telemetry. */
static cloud_time_telemetry_t app_collect_cloud_time_telemetry(void);

/** @brief Convert and forward sensor manager snapshots to the GUI queue. */
static void app_sensor_status_callback(
    const sensor_manager_status_t *status,
    void *user_context);

/** @brief Map an audio manager pipeline state to its compact GUI equivalent. */
static ui_audio_state_t app_map_audio_state(
    audio_manager_state_t state);

/** @brief Copy audio status into the GUI queue without calling LVGL. */
static void app_audio_status_callback(
    const audio_manager_status_t *status,
    void *user_context);

/** @brief Map a cloud manager state to its application GUI equivalent. */
static ui_cloud_state_t app_map_cloud_state(
    cloud_manager_state_t state);

/** @brief Convert and forward cloud manager snapshots to the GUI queue. */
static void app_cloud_status_callback(
    const cloud_manager_status_t *status,
    void *user_context);

/**
 * @brief Convert one Wi-Fi manager snapshot into a coordinator runtime event.
 *
 * @param[in] status Wi-Fi manager status snapshot.
 * @param[out] event Converted coordinator event.
 *
 * @return true when the snapshot represents a runtime event.
 */
static bool app_map_wifi_status_to_network_event(
    const wifi_manager_status_t *status,
    app_network_coordinator_wifi_event_t *event);

/**
 * @brief Check whether BLE cleanup and network ownership permit cloud start.
 *
 * @param[in] state Current application network coordinator state.
 *
 * @return true when the cloud task may be created without overlapping BLE
 *         provisioning.
 */
static bool app_network_state_allows_cloud_start(
    app_network_coordinator_state_t state);

/**
 * @brief Check whether audio startup may claim its runtime resources.
 *
 * The audio manager remains uninitialized while BLE provisioning or the
 * Station IPv4 handoff is active. This application-level gate keeps those
 * temporary network phases independent from audio I2S/DMA/task allocation.
 *
 * @param[in] state Current application network coordinator state.
 *
 * @return true only after the coordinator has completed network handoff.
 */
static bool app_network_state_allows_audio_start(
    app_network_coordinator_state_t state);

/**
 * @brief Initialize and start audio from the app_main lifecycle context.
 *
 * The caller must first satisfy app_network_state_allows_audio_start(). This
 * helper must not be called from a Wi-Fi or provisioning callback.
 *
 * @return ESP_OK when the audio manager task reaches IDLE; otherwise an audio
 *         manager initialization or startup error.
 */
static esp_err_t app_start_audio_manager_after_network_online(void);

/* Application -------------------------------------------------------------- */
/** @brief Initialize the current application services and run diagnostics. */
void app_main(void)
{
    /* Project identity is emitted first to make firmware logs traceable. */
    ESP_LOGI(TAG, "PROJECT: %s", APP_PROJECT_NAME);
    ESP_LOGI(TAG, "VERSION: %s", APP_PROJECT_VER);
    ESP_LOGI(TAG, "BUILD DATE: %s", APP_PROJECT_VER_DATE);

    esp_err_t network_ret =
        network_platform_init();

    if (network_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize network platform: %s",
            esp_err_to_name(network_ret));

        return;
    }

    const time_manager_config_t time_config =
        time_manager_default_config();
    esp_err_t time_ret =
        time_manager_init(&time_config);

    if (time_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize time manager: %s; continuing without time sync",
            esp_err_to_name(time_ret));
    }
    else
    {
        time_ret = time_manager_start();

        if (time_ret != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Failed to configure time manager: %s; continuing without time sync",
                esp_err_to_name(time_ret));
        }
    }

    esp_err_t ret =
        display_driver_init(
            &display_handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize display driver: %s",
            esp_err_to_name(ret));

        return;
    }

    ret =
        ui_manager_lvgl_init(
            &display_handle);

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize LVGL UI manager: %s",
            esp_err_to_name(ret));

        return;
    }

    /*
     * SD initialization is intentionally split from the recovery task. This
     * lets LVGL register S: while offline, then lets the BOOT screen appear
     * before a cold or slow SD card begins its retry sequence.
     */
    const esp_err_t sd_init_ret = sd_card_manager_init();

    if (sd_init_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize SD recovery service: %s; continuing without SD",
            esp_err_to_name(sd_init_ret));
    }

    ret = lvgl_sd_fs_register();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to register LVGL SD filesystem: %s",
            esp_err_to_name(ret));

        return;
    }

    ret = app_gui_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize application GUI: %s",
            esp_err_to_name(ret));

        return;
    }

    ESP_LOGD(TAG, "Starting LVGL task handler");

    ret = app_gui_start_ui_task();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start application GUI task: %s",
            esp_err_to_name(ret));

        return;
    }

    ESP_LOGI(TAG, "LVGL display initialized successfully");

    ret = app_gui_request_screen(APP_GUI_SCREEN_BOOT);

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to request BOOT screen before SD recovery: %s",
            esp_err_to_name(ret));

        return;
    }

    /*
     * Give the UI task one scheduled frame to draw its built-in, SD-free boot
     * screen. No LVGL API is called from app_main after the task has started.
     */
    vTaskDelay(pdMS_TO_TICKS(50U));

    if (sd_init_ret == ESP_OK)
    {
        const esp_err_t sd_start_ret = sd_card_manager_start();

        if (sd_start_ret != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Failed to start SD recovery task: %s; continuing without SD",
                esp_err_to_name(sd_start_ret));
        }
    }

#if PERFORMANCE_MONITOR
    esp_err_t monitor_ret =
        performance_monitor_start();

    if (monitor_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start performance monitor: %s",
            esp_err_to_name(monitor_ret));
    }
#endif

    /*
     * Phase 7.2 reset-input path.
     *
     * The reset coordinator must be running before button_manager starts so
     * that the first callback always has a valid non-blocking destination.
     *
     * A failure disables only the physical factory-reset path. LCD, sensor,
     * Wi-Fi, provisioning, and cloud initialization continue normally.
     */
    esp_err_t reset_ret =
        app_reset_coordinator_init();

    if (reset_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize reset coordinator: %s",
            esp_err_to_name(reset_ret));
    }
    else
    {
        reset_ret =
            app_reset_coordinator_start();

        if (reset_ret != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Failed to start reset coordinator: %s",
                esp_err_to_name(reset_ret));
        }
        else
        {
            esp_err_t button_ret =
                button_manager_init(
                    &BUTTON_MANAGER_CONFIG);

            if (button_ret != ESP_OK)
            {
                ESP_LOGE(
                    TAG,
                    "Failed to initialize button manager: %s",
                    esp_err_to_name(button_ret));
            }
            else
            {
                button_ret =
                    button_manager_register_callback(
                        app_button_event_callback,
                        NULL);

                if (button_ret != ESP_OK)
                {
                    ESP_LOGE(
                        TAG,
                        "Failed to register button callback: %s",
                        esp_err_to_name(button_ret));
                }
                else
                {
                    button_ret =
                        button_manager_start();

                    if (button_ret != ESP_OK)
                    {
                        ESP_LOGE(
                            TAG,
                            "Failed to start button manager: %s",
                            esp_err_to_name(button_ret));
                    }
                }
            }
        }
    }

    esp_err_t wifi_ret =
        wifi_manager_init();

    if (wifi_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize Wi-Fi manager: %s",
            esp_err_to_name(wifi_ret));

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

    if (callback_ret != ESP_OK)
    {
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

    /*
     * Prepare authentication state and the telemetry queue before the sensor
     * producer can invoke its callback. These initialization APIs do not
     * perform TLS requests or create the cloud task.
     */
    esp_err_t service_ret =
        firebase_auth_init(
            &FIREBASE_AUTH_CONFIG);

    if (service_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize Firebase Authentication: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret =
        cloud_manager_init(
            &CLOUD_MANAGER_CONFIG);

    if (service_ret != ESP_OK)
    {
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

    if (service_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to register cloud manager status callback: %s",
            esp_err_to_name(service_ret));
        return;
    }

    /*
     * Sensor sampling is a local service. It starts independently of network
     * availability, with both GUI and cloud destination queues already ready.
     */
    service_ret =
        sensor_manager_init(
            &SENSOR_MANAGER_CONFIG);

    if (service_ret != ESP_OK)
    {
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

    if (service_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to register sensor callback: %s",
            esp_err_to_name(service_ret));
        return;
    }

    service_ret = sensor_manager_start();

    if (service_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start sensor manager: %s",
            esp_err_to_name(service_ret));
        return;
    }

    /*
     * Start network boot only after all local producers and deferred-consumer
     * queues are ready. The coordinator task may now connect stored Wi-Fi or
     * run bounded BLE provisioning without blocking app_main().
     */
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

    bool cloud_started = false;
    /* Preserve the prior one-shot audio lifecycle attempt after the new gate. */
    bool audio_start_attempted = false;

    while (1)
    {
        if (!cloud_started || !audio_start_attempted)
        {
            app_network_coordinator_state_t network_state =
                APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED;

            service_ret =
                app_network_coordinator_get_state(
                    &network_state);

            if (service_ret != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "Failed to inspect network readiness for deferred startup: %s",
                    esp_err_to_name(service_ret));
            }
            else if (!audio_start_attempted &&
                     app_network_state_allows_audio_start(
                         network_state))
            {
                audio_start_attempted = true;

                const esp_err_t audio_ret =
                    app_start_audio_manager_after_network_online();

                if (audio_ret == ESP_OK)
                {
                    ESP_LOGI(
                        TAG,
                        "Audio manager started after network handoff: state=%s",
                        app_network_coordinator_state_to_string(
                            network_state));
                }
                else
                {
                    ESP_LOGE(
                        TAG,
                        "Audio manager startup after network handoff failed: %s",
                        esp_err_to_name(audio_ret));
                }
            }
            else if (!cloud_started &&
                     app_network_state_allows_cloud_start(
                         network_state))
            {
                service_ret =
                    cloud_manager_start();

                if (service_ret == ESP_OK)
                {
                    cloud_started = true;

                    ESP_LOGI(
                        TAG,
                        "Cloud manager started after network handoff: state=%s",
                        app_network_coordinator_state_to_string(
                            network_state));
                }
                else
                {
                    /*
                     * cloud_manager_start() rolls back its started flag when
                     * task allocation fails, so a later loop may retry after
                     * temporary memory pressure has cleared.
                     */
                    ESP_LOGW(
                        TAG,
                        "Cloud manager start deferred: %s",
                        esp_err_to_name(service_ret));
                }
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                (cloud_started && audio_start_attempted)
                    ? 5000U
                    : 500U));
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

    if (status == NULL)
    {
        return;
    }

    app_network_coordinator_wifi_event_t network_event;

    if (app_map_wifi_status_to_network_event(
            status,
            &network_event))
    {
        const esp_err_t coordinator_error =
            app_network_coordinator_notify_wifi_event(
                network_event,
                status->disconnect_reason);

        if (coordinator_error != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Failed to forward Wi-Fi event to coordinator: %s",
                esp_err_to_name(coordinator_error));
        }
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
        status->ssid);

    snprintf(
        ui_status.ipv4_address,
        sizeof(ui_status.ipv4_address),
        "%s",
        status->ipv4_address);

    const esp_err_t ret =
        app_gui_post_wifi_status(
            &ui_status);

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to forward Wi-Fi status to UI: %s",
            esp_err_to_name(ret));
    }

    const esp_err_t cloud_network_error =
        cloud_manager_notify_network_state(
            status->has_ipv4_address);

    if ((cloud_network_error != ESP_OK) &&
        (cloud_network_error !=
         ESP_ERR_INVALID_STATE))
    {
        ESP_LOGW(
            TAG,
            "Failed to forward Wi-Fi state to cloud manager: %s",
            esp_err_to_name(cloud_network_error));
    }

    const esp_err_t time_network_error =
        time_manager_notify_network_state(
            status->has_ipv4_address);

    if ((time_network_error != ESP_OK) &&
        (time_network_error !=
         ESP_ERR_INVALID_STATE))
    {
        ESP_LOGW(
            TAG,
            "Failed to forward Wi-Fi state to time manager: %s",
            esp_err_to_name(time_network_error));
    }

    ESP_LOGD(
        TAG,
        "Wi-Fi callback: state=%s, ip=%s, reason=%u",
        wifi_manager_state_to_string(status->state),
        status->has_ipv4_address
            ? status->ipv4_address
            : "<none>",
        (unsigned int)status->disconnect_reason);
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

static ui_audio_state_t app_map_audio_state(
    audio_manager_state_t state)
{
    switch (state)
    {
        case AUDIO_MANAGER_STATE_INITIALIZED:
            return UI_AUDIO_STATE_READY;

        case AUDIO_MANAGER_STATE_IDLE:
            return UI_AUDIO_STATE_IDLE;

        case AUDIO_MANAGER_STATE_RECORDING:
            return UI_AUDIO_STATE_RECORDING;

        case AUDIO_MANAGER_STATE_PROCESSING:
            return UI_AUDIO_STATE_PROCESSING;

        case AUDIO_MANAGER_STATE_PLAYBACK:
            return UI_AUDIO_STATE_PLAYBACK;

        case AUDIO_MANAGER_STATE_ERROR:
            return UI_AUDIO_STATE_ERROR;

        case AUDIO_MANAGER_STATE_UNINITIALIZED:
        default:
            return UI_AUDIO_STATE_UNAVAILABLE;
    }
}

static void app_audio_status_callback(
    const audio_manager_status_t *status,
    void *user_context)
{
    (void)user_context;

    if (status == NULL)
    {
        return;
    }

    const ui_audio_status_t ui_status =
    {
        .state = app_map_audio_state(status->state),
        .last_error = status->last_error,
    };

    const esp_err_t ret = app_gui_post_audio_status(&ui_status);

    if (ret != ESP_OK)
    {
        ESP_LOGD(
            TAG,
            "Audio GUI update dropped: %s",
            esp_err_to_name(ret));
    }
}

static cloud_time_telemetry_t app_collect_cloud_time_telemetry(void)
{
    cloud_time_telemetry_t cloud_time = {0};
    time_manager_status_t time_status = {0};

    if ((time_manager_get_status(&time_status) != ESP_OK) ||
        !time_status.synced ||
        (time_status.last_sync_unix <= (time_t)0)) {
        return cloud_time;
    }

    time_t current_unix = (time_t)0;
    char local_time[CLOUD_TIME_LOCAL_ISO8601_BUFFER_SIZE] = {0};

    if ((time_manager_get_unix_time(&current_unix) != ESP_OK) ||
        (current_unix <= (time_t)0) ||
        (time_manager_format_iso8601(
             local_time,
             sizeof(local_time)) != ESP_OK)) {
        return cloud_time;
    }

    cloud_time.synced = true;
    cloud_time.unix_time = current_unix;
    cloud_time.last_sync_unix = time_status.last_sync_unix;
    memcpy(
        cloud_time.local_time,
        local_time,
        sizeof(cloud_time.local_time));

    return cloud_time;
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
        app_gui_post_sensor_status(
            &ui_status);

    if (error != ESP_OK)
    {
        ESP_LOGD(
            TAG,
            "Sensor GUI update dropped: %s",
            esp_err_to_name(error));
    }

    audio_manager_status_t audio_status = {0};
    const esp_err_t audio_status_ret =
        audio_manager_get_status(&audio_status);

    cloud_audio_state_t cloud_audio_state =
        CLOUD_AUDIO_STATE_UNAVAILABLE;
    esp_err_t cloud_audio_error =
        audio_status_ret;

    if (audio_status_ret == ESP_OK)
    {
        cloud_audio_error =
            audio_status.last_error;

        switch (audio_status.state)
        {
            case AUDIO_MANAGER_STATE_INITIALIZED:
                cloud_audio_state =
                    CLOUD_AUDIO_STATE_READY;
                break;

            case AUDIO_MANAGER_STATE_IDLE:
                cloud_audio_state =
                    CLOUD_AUDIO_STATE_IDLE;
                break;

            case AUDIO_MANAGER_STATE_RECORDING:
                cloud_audio_state =
                    CLOUD_AUDIO_STATE_RECORDING;
                break;

            case AUDIO_MANAGER_STATE_PROCESSING:
                cloud_audio_state =
                    CLOUD_AUDIO_STATE_PROCESSING;
                break;

            case AUDIO_MANAGER_STATE_PLAYBACK:
                cloud_audio_state =
                    CLOUD_AUDIO_STATE_PLAYBACK;
                break;

            case AUDIO_MANAGER_STATE_ERROR:
                cloud_audio_state =
                    CLOUD_AUDIO_STATE_ERROR;
                break;

            case AUDIO_MANAGER_STATE_UNINITIALIZED:
            default:
                cloud_audio_state =
                    CLOUD_AUDIO_STATE_UNAVAILABLE;
                break;
        }
    }

    const cloud_time_telemetry_t cloud_time =
        app_collect_cloud_time_telemetry();

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

        .audio =
        {
            .state =
                cloud_audio_state,

            .last_error =
                cloud_audio_error,
        },

        .time = cloud_time,
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
        app_gui_post_cloud_status(
            &ui_status);

    if (error != ESP_OK)
    {
        ESP_LOGD(
            TAG,
            "Cloud GUI update dropped: %s",
            esp_err_to_name(error));
    }
}

static bool app_map_wifi_status_to_network_event(
    const wifi_manager_status_t *status,
    app_network_coordinator_wifi_event_t *event)
{
    if ((status == NULL) ||
        (event == NULL))
    {
        return false;
    }

    switch (status->state)
    {
        case WIFI_MANAGER_STATE_CONNECTING:
            *event =
                APP_NETWORK_COORDINATOR_WIFI_EVENT_CONNECTING;
            return true;

        case WIFI_MANAGER_STATE_WAITING_FOR_IP:
            *event =
                APP_NETWORK_COORDINATOR_WIFI_EVENT_WAITING_FOR_IP;
            return true;

        case WIFI_MANAGER_STATE_CONNECTED:
            *event =
                status->has_ipv4_address
                    ? APP_NETWORK_COORDINATOR_WIFI_EVENT_ONLINE
                    : APP_NETWORK_COORDINATOR_WIFI_EVENT_WAITING_FOR_IP;
            return true;

        case WIFI_MANAGER_STATE_DISCONNECTED:
        case WIFI_MANAGER_STATE_RETRY_WAIT:
        case WIFI_MANAGER_STATE_FAILED:
            *event =
                APP_NETWORK_COORDINATOR_WIFI_EVENT_OFFLINE;
            return true;

        case WIFI_MANAGER_STATE_UNINITIALIZED:
        case WIFI_MANAGER_STATE_READY:
        default:
            return false;
    }
}

static bool app_network_state_allows_cloud_start(
    app_network_coordinator_state_t state)
{
    /*
     * Stored credentials enter CONNECTING without BLE. Provisioning remains
     * PROVISIONING until BLE cleanup and active-connection adoption complete,
     * then enters ONLINE. No other lifecycle state may allocate the TLS task.
     */
    return
        (state ==
         APP_NETWORK_COORDINATOR_STATE_CONNECTING) ||
        (state ==
         APP_NETWORK_COORDINATOR_STATE_ONLINE);
}

static bool app_network_state_allows_audio_start(
    app_network_coordinator_state_t state)
{
    /*
     * Unlike cloud startup for stored credentials, audio must not allocate
     * I2S/DMA/task resources while Station association or IPv4 acquisition is
     * still in progress. For provisioning, ONLINE also proves BLE cleanup and
     * active-connection adoption have completed.
     */
    return
        state ==
        APP_NETWORK_COORDINATOR_STATE_ONLINE;
}

static esp_err_t app_start_audio_manager_after_network_online(void)
{
    /*
     * app_main only composes the audio component. Its production task reaches
     * IDLE and privately owns commands, WAV streams, and all I2S operations.
     */
    const audio_manager_config_t audio_config =
        audio_manager_default_config();

    esp_err_t audio_ret =
        audio_manager_init(&audio_config);

    if (audio_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize audio manager: %s",
            esp_err_to_name(audio_ret));

        return audio_ret;
    }

    const esp_err_t audio_gui_ret =
        audio_manager_register_status_callback(
            app_audio_status_callback,
            NULL);

    if (audio_gui_ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to register audio GUI status callback: %s",
            esp_err_to_name(audio_gui_ret));
    }

    audio_ret = audio_manager_start();

    if (audio_ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to start audio manager task: %s",
            esp_err_to_name(audio_ret));

        return audio_ret;
    }

    ESP_LOGI(
        TAG,
        "Audio manager task started; mode is reported by audio_manager");

#if CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST
    const esp_err_t audio_test_ret =
        app_audio_api_test_task_start();

    if (audio_test_ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to start public audio API validation task: %s",
            esp_err_to_name(audio_test_ret));
    }
    else
    {
        ESP_LOGI(
            TAG,
            "Public audio API validation task started at priority 6");
    }
#endif

    return ESP_OK;
}

static void app_button_event_callback(
    const button_manager_event_data_t *event_data,
    void *user_context)
{
    (void)user_context;

    if (event_data == NULL)
    {
        return;
    }

    app_reset_coordinator_input_event_t reset_input_event;

    switch (event_data->event)
    {
        case BUTTON_MANAGER_EVENT_PRESSED:
            ESP_LOGD(TAG, "Button pressed");

            reset_input_event =
                APP_RESET_COORDINATOR_INPUT_PRESSED;
            break;

        case BUTTON_MANAGER_EVENT_RELEASED:
            ESP_LOGD(
                TAG,
                "Button released: held=%" PRIu32 " ms",
                event_data->held_ms);
            reset_input_event =
                APP_RESET_COORDINATOR_INPUT_RELEASED;
            break;

        case BUTTON_MANAGER_EVENT_LONG_PRESS:
            ESP_LOGI(
                TAG,
                "Button long press detected: held=%" PRIu32 " ms",
                event_data->held_ms);
            reset_input_event =
                APP_RESET_COORDINATOR_INPUT_LONG_PRESS;
            break;

        default:
            ESP_LOGW(
                TAG,
                "Unknown button event: %d",
                (int)event_data->event);
            return;
    }

    /*
     * Non-blocking queue post only.
     *
     * No storage access, LVGL call, provisioning action, or reboot occurs in
     * button task context.
     */
    const esp_err_t post_ret =
        app_reset_coordinator_post_input_event(
            reset_input_event);

    if (post_ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to forward button event to reset coordinator: %s",
            esp_err_to_name(post_ret));
    }
}
