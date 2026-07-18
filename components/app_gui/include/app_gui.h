#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Common GUI Types -------------------------------------------------------- */

/** @brief Application screen tracked independently of the LVGL root object. */
typedef enum
{
    /** No application screen is currently active. */
    APP_GUI_SCREEN_NONE = 0,

    /** Wi-Fi status screen. */
    APP_GUI_SCREEN_WIFI,

    /** Temperature and humidity sensor screen. */
    APP_GUI_SCREEN_SENSOR,
} app_gui_screen_id_t;

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

    float temperature_c;
    float humidity_percent;

    bool data_valid;
    bool data_stale;

    esp_err_t last_error;
} ui_sensor_status_t;

/* Lifecycle API ----------------------------------------------------------- */

/**
 * @brief Initialize the application GUI status queues.
 *
 * LVGL must already be initialized by ui_manager_lvgl_init(). Screen widgets
 * are created separately by app_gui_create_wifi_screen() or
 * app_gui_create_sensor_screen().
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already initialized,
 *         or ESP_ERR_INVALID_ARG if a queue cannot be created.
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

/* Wi-Fi Screen API -------------------------------------------------------- */

/**
 * @brief Create and activate the Wi-Fi status screen.
 *
 * This function acquires and releases the LVGL mutex internally.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if LVGL has no active
 *         screen, or ESP_ERR_NO_MEM if a widget cannot be created.
 */
esp_err_t app_gui_create_wifi_screen(void);

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

/* Sensor Screen API ------------------------------------------------------- */

/**
 * @brief Create and activate the temperature and humidity sensor screen.
 *
 * This function acquires and releases the LVGL mutex internally. Sensor
 * status updates posted through app_gui_post_sensor_status() are applied by
 * the GUI task while this screen is active.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if LVGL has no active
 *         screen, or ESP_ERR_NO_MEM if a widget cannot be created.
 */
esp_err_t app_gui_create_sensor_screen(void);

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

/* Screen Management API --------------------------------------------------- */

/**
 * @brief Set the ID of the currently active application screen.
 *
 * This API updates the tracked screen ID only. It does not create, load, or
 * delete an LVGL screen.
 *
 * @param screen_id Screen ID to store.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG for an unknown ID.
 */
esp_err_t app_gui_set_screen_id(
    app_gui_screen_id_t screen_id);

/**
 * @brief Get a thread-safe snapshot of the active application screen ID.
 *
 * @param[out] screen_id Receives the current screen ID.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG if screen_id is NULL.
 */
esp_err_t app_gui_get_screen_id(
    app_gui_screen_id_t *screen_id);

/**
 * @brief Replace the active LVGL screen with an empty application screen.
 *
 * The caller must already own the LVGL mutex, or call this function from an
 * LVGL callback executed by the mutex-protected app GUI task. Do not acquire
 * the same non-recursive mutex again from such a callback.
 *
 * @return ESP_OK on success, or ESP_ERR_INVALID_RESPONSE if no active screen
 *         exists or the replacement screen cannot be created.
 */
esp_err_t app_gui_clear_screen(void);

/* Demo API ---------------------------------------------------------------- */

/**
 * @brief Create a simple screen with a centered "LVGL OK" label.
 *
 * This function acquires and releases the LVGL mutex internally.
 */
void app_gui_create_demo_screen(void);

/**
 * @brief Start the optional moving-label demonstration task.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before app_gui_init() or
 *         when the demo task is already running, or ESP_ERR_NO_MEM if task
 *         creation fails.
 */
esp_err_t app_gui_start_running_demo_task(void);
