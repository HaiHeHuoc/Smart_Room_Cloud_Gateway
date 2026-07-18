#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"


#define UI_WIFI_SSID_BUFFER_SIZE  33U
#define UI_WIFI_IPV4_BUFFER_SIZE  16U

typedef enum
{
    UI_WIFI_STATE_IDLE = 0,
    UI_WIFI_STATE_CONNECTING,
    UI_WIFI_STATE_WAITING_FOR_IP,
    UI_WIFI_STATE_CONNECTED,
    UI_WIFI_STATE_DISCONNECTED,
    UI_WIFI_STATE_FAILED,
} ui_wifi_state_t;

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

typedef enum
{
    UI_SENSOR_STATE_INITIALIZING = 0,
    UI_SENSOR_STATE_READY,
    UI_SENSOR_STATE_DEGRADED,
    UI_SENSOR_STATE_ERROR
} ui_sensor_state_t;

typedef enum
{
    APP_GUI_SCREEN_WIFI,
    APP_GUI_SCREEN_SENSOR,
} app_gui_screen_id_t;

typedef struct
{
    ui_sensor_state_t state;

    float temperature_c;
    float humidity_percent;

    bool data_valid;
    bool data_stale;

    esp_err_t last_error;
} ui_sensor_status_t;

/**
 * @brief Initialize the Wi-Fi status queue and create the Wi-Fi status screen.
 *
 * LVGL must already be initialized by ui_manager_lvgl_init().
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already initialized,
 *         or ESP_ERR_NO_MEM if queue allocation fails.
 */
esp_err_t app_gui_init(void);

/**
 * @brief Start the main application GUI and LVGL timer task.
 *
 * app_gui_init() and ui_manager_lvgl_init() must be called first.
 *
 * @return ESP_OK on success, or ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t app_gui_start_ui_task(void);

/**
 * @brief Send the latest Wi-Fi status to the application GUI task.
 *
 * This function does not call LVGL directly.
 * app_gui_init() must be called first.
 */
esp_err_t app_gui_post_wifi_status(
    const ui_wifi_status_t *status);

/**
 * @brief Demo function to create a simple screen with a label displaying "LVGL OK".
 * 
 */
void app_gui_create_demo_screen(void);

/**
 * @brief Start running demo task
 *
 * app_gui_init() must be called first.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized, or
 *         ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t app_gui_start_running_demo_task(void);

esp_err_t app_gui_create_wifi_screen(void);

esp_err_t app_gui_post_sensor_status(
    const ui_sensor_status_t *status);