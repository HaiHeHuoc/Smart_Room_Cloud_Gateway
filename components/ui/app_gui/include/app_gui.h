#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Common GUI Types -------------------------------------------------------- */

/** @brief Logical application screen owned by the app_gui UI task. */
typedef enum
{
    /** No application screen is currently active. */
    APP_GUI_SCREEN_NONE = 0,

    /** Minimal startup placeholder for configured and safe-error boot paths. */
    APP_GUI_SCREEN_BOOT,

    /** Minimal BLE Wi-Fi provisioning placeholder. */
    APP_GUI_SCREEN_PROVISIONING,

    /** Wi-Fi status screen. */
    APP_GUI_SCREEN_WIFI_STATUS,

    /** Temperature and humidity sensor screen. */
    APP_GUI_SCREEN_SENSOR_DASHBOARD,
} app_gui_screen_id_t;

/* Provisioning UI Types -------------------------------------------------- */

/** Storage size for one null-terminated BLE provisioning QR payload. */
#define UI_PROVISIONING_QR_PAYLOAD_BUFFER_SIZE 192U

/** @brief UI-only states rendered by the BLE Wi-Fi provisioning screen. */
typedef enum
{
    UI_PROVISIONING_STATE_STARTING = 0,
    UI_PROVISIONING_STATE_WAITING_FOR_PHONE,
    UI_PROVISIONING_STATE_CREDENTIAL_RECEIVED,
    UI_PROVISIONING_STATE_CONNECTING_WIFI,
    UI_PROVISIONING_STATE_WAITING_FOR_IP,
    UI_PROVISIONING_STATE_SAVING_CONFIG,
    UI_PROVISIONING_STATE_CLEANING_UP,
    UI_PROVISIONING_STATE_SUCCESS,
    UI_PROVISIONING_STATE_FAILED,
    UI_PROVISIONING_STATE_TIMEOUT,
    UI_PROVISIONING_STATE_RETRYING,
} ui_provisioning_state_t;

/**
 * @brief Non-sensitive provisioning snapshot copied into the GUI command queue.
 *
 * This UI model contains no credentials, PoP value, framework-owned pointer,
 * or raw Wi-Fi configuration.
 */
typedef struct
{
    ui_provisioning_state_t state;
    esp_err_t last_error;
    uint16_t wifi_disconnect_reason;
} ui_provisioning_status_t;

/**
 * @brief Caller-owned provisioning QR payload copied into the GUI queue.
 *
 * The payload can contain a Security 1 PoP and must not be logged.
 */
typedef struct
{
    char payload[UI_PROVISIONING_QR_PAYLOAD_BUFFER_SIZE];
} ui_provisioning_qr_payload_t;

/* Wi-Fi UI Types ---------------------------------------------------------- */

#define UI_WIFI_SSID_BUFFER_SIZE  33U
#define UI_WIFI_IPV4_BUFFER_SIZE  16U

/** @brief Wi-Fi states rendered by the application GUI. */
typedef enum
{
    UI_WIFI_STATE_IDLE = 0,
    UI_WIFI_STATE_CONNECTING,
    UI_WIFI_STATE_WAITING_FOR_IP,
    UI_WIFI_STATE_CONNECTED,
    UI_WIFI_STATE_DISCONNECTED,
    UI_WIFI_STATE_FAILED,
    UI_WIFI_STATE_RETRY_WAIT
} ui_wifi_state_t;

/** @brief Application-owned copy of the Wi-Fi status used by the GUI task. */
typedef struct
{
    ui_wifi_state_t state;

    char ssid[UI_WIFI_SSID_BUFFER_SIZE];
    char ipv4_address[UI_WIFI_IPV4_BUFFER_SIZE];

    int8_t rssi_dbm;
    uint16_t disconnect_reason;

    bool has_ipv4_address;
    bool rssi_valid;
} ui_wifi_status_t;

/* Sensor UI Types --------------------------------------------------------- */

/** @brief Sensor states rendered by the application GUI. */
typedef enum
{
    UI_SENSOR_STATE_INITIALIZING = 0,
    UI_SENSOR_STATE_READY,
    UI_SENSOR_STATE_DEGRADED,
    UI_SENSOR_STATE_ERROR
} ui_sensor_state_t;

/** @brief Sensor snapshot copied from sensor_manager into the GUI queue. */
typedef struct
{
    ui_sensor_state_t state;

    /** Temperature copied from sensor_manager; -1.0f marks a failed read. */
    float temperature_c;

    /** Humidity copied from sensor_manager; -1.0f marks a failed read. */
    float humidity_percent;

    /** Whether sensor_manager has recorded at least one successful sample. */
    bool data_valid;

    /** Whether the latest successful sample has exceeded its stale timeout. */
    bool data_stale;

    /** Result of the most recent sensor read. */
    esp_err_t last_error;
} ui_sensor_status_t;

/* Cloud UI Types ---------------------------------------------------------- */

/** @brief Cloud states rendered by the Smart Room dashboard. */
typedef enum
{
    UI_CLOUD_STATE_UNKNOWN = 0,
    UI_CLOUD_STATE_WAITING,
    UI_CLOUD_STATE_UPLOADING,
    UI_CLOUD_STATE_ONLINE,
    UI_CLOUD_STATE_RETRY_WAIT,
    UI_CLOUD_STATE_AUTH_ERROR,
    UI_CLOUD_STATE_ERROR
} ui_cloud_state_t;

/** @brief Cloud status snapshot copied into the GUI queue. */
typedef struct
{
    ui_cloud_state_t state;
    esp_err_t last_error;
    int last_http_status;
} ui_cloud_status_t;

/* Lifecycle API ----------------------------------------------------------- */

/**
 * @brief Initialize the application GUI command and status queues.
 *
 * LVGL must already be initialized by ui_manager_lvgl_init(). Screen widgets
 * are created asynchronously by the app_gui UI task after a screen request.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already initialized,
 *         or ESP_ERR_NO_MEM if a queue cannot be created.
 */
esp_err_t app_gui_init(void);

/**
 * @brief Start the main application GUI and LVGL timer task.
 *
 * app_gui_init() and ui_manager_lvgl_init() must be called first.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if app_gui_init() has not
 *         completed or the task is already running, or ESP_ERR_NO_MEM if task
 *         creation fails.
 */
esp_err_t app_gui_start_ui_task(void);

/* Provisioning Status API ------------------------------------------------ */

/**
 * @brief Post a copied provisioning status to the GUI task without waiting.
 *
 * The status is copied into a dedicated length-one latest-value queue. This
 * function never calls LVGL, does not retain @p status, and is safe from
 * normal task and task-context callback code. It is not ISR-safe and never
 * changes the active screen.
 *
 * @param[in] status Non-sensitive provisioning UI snapshot to copy.
 * @return ESP_OK when queued, ESP_ERR_INVALID_ARG for NULL or an invalid
 *         provisioning state, ESP_ERR_INVALID_STATE before app_gui_init(), or
 *         ESP_FAIL when the queue update fails unexpectedly.
 */
esp_err_t app_gui_post_provisioning_status(
    const ui_provisioning_status_t *status);

/**
 * @brief Replace the pending provisioning QR payload without waiting.
 *
 * This function validates and copies the complete payload into a dedicated
 * latest-value queue. It never retains @p payload, calls LVGL, or changes the
 * active screen. It is safe from normal task and task-context callback code,
 * but is not ISR-safe.
 *
 * @param[in] payload Null-terminated provisioning QR payload to copy.
 * @return ESP_OK when copied, ESP_ERR_INVALID_ARG for NULL, empty, or
 *         unterminated input, ESP_ERR_INVALID_STATE before app_gui_init(), or
 *         ESP_FAIL when the queue update fails.
 */
esp_err_t app_gui_post_provisioning_qr_payload(
    const ui_provisioning_qr_payload_t *payload);

/**
 * @brief Invalidate the session-owned provisioning QR payload.
 *
 * This posts an explicit unavailable message to the GUI task. The GUI task
 * securely clears its cached payload and hides the QR widget if the
 * provisioning screen is active. Generic screen transitions do not clear the
 * session payload.
 *
 * This function never calls LVGL and is safe from normal task and
 * task-context callback code. It is not ISR-safe.
 *
 * @return ESP_OK when queued, ESP_ERR_INVALID_STATE before app_gui_init(), or
 *         ESP_FAIL when the queue update fails unexpectedly.
 */
esp_err_t app_gui_clear_provisioning_qr_payload(void);

/* Wi-Fi Status API -------------------------------------------------------- */
/**
 * @brief Send the latest Wi-Fi status to the application GUI task.
 *
 * This function does not call LVGL directly and does not wait. Because the
 * Wi-Fi queue has length one, a pending status is overwritten by the newest
 * snapshot.
 *
 * @param status Wi-Fi status snapshot copied into the GUI queue.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL,
 *         ESP_ERR_INVALID_STATE before app_gui_init(), or ESP_FAIL if the
 *         queue update fails.
 */
esp_err_t app_gui_post_wifi_status(
    const ui_wifi_status_t *status);

/* Sensor Status API ------------------------------------------------------- */
/**
 * @brief Post a sensor status snapshot to the GUI task without waiting.
 *
 * @param status Sensor status copied into the GUI queue.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if status is NULL,
 *         ESP_ERR_INVALID_STATE before app_gui_init(), or ESP_ERR_TIMEOUT
 *         when the queue is full.
 */
esp_err_t app_gui_post_sensor_status(
    const ui_sensor_status_t *status);

/**
 * @brief Replace the pending cloud status without waiting.
 *
 * This function copies the snapshot into a queue of length one and never calls
 * LVGL. It is safe to call from the cloud manager status callback.
 *
 * @param[in] status Cloud status snapshot to copy.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for NULL,
 *         ESP_ERR_INVALID_STATE before app_gui_init(), or ESP_FAIL when the
 *         queue update fails.
 */
esp_err_t app_gui_post_cloud_status(
    const ui_cloud_status_t *status);

/* Screen Routing API ------------------------------------------------------ */

/**
 * @brief Request an application screen transition without waiting.
 *
 * The request is copied to the GUI command queue. Only the app_gui UI task
 * creates, renders, or replaces LVGL screens. This function never calls LVGL,
 * does not wait for screen construction, and is safe from normal task and
 * task-context callback code. It is not ISR-safe.
 *
 * @param[in] screen_id BOOT, PROVISIONING, WIFI_STATUS, or
 *            SENSOR_DASHBOARD. NONE is not an external target.
 * @return ESP_OK when queued, ESP_ERR_INVALID_ARG for an invalid target,
 *         ESP_ERR_INVALID_STATE before app_gui_init(), or ESP_ERR_TIMEOUT
 *         when the command queue is full.
 */
esp_err_t app_gui_request_screen(
    app_gui_screen_id_t screen_id);

/**
 * @brief Get a thread-safe snapshot of the active application screen ID.
 *
 * @param[out] screen_id Receives the current screen ID.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG if screen_id is NULL.
 */
esp_err_t app_gui_get_screen_id(
    app_gui_screen_id_t *screen_id);
