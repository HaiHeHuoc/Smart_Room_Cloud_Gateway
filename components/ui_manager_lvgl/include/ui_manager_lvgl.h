#pragma once

#include "esp_err.h"
#include "display_driver.h"

#include "lvgl.h"

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

/**
 * @brief Send the latest Wi-Fi status to the UI task.
 *
 * This function does not call LVGL directly.
 */
esp_err_t ui_manager_lvgl_post_wifi_status(
    const ui_wifi_status_t *status);

/**
 * @brief Initialize LVGL, display buffers, flush synchronization, and tick timer.
 *
 * @param[in] diplay_handle Initialized display driver handle. The manager
 *            borrows this pointer, so its storage must remain valid.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid display handle,
 *         ESP_ERR_NO_MEM on allocation failure, or an ESP-IDF timer/LCD error.
 */
esp_err_t ui_manager_lvgl_init(display_driver_handle_t* diplay_handle);

/**
 * @brief Start the main LVGL timer and rendering task.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if task creation fails.
 */
esp_err_t ui_manager_lvgl_start_UI_task(void);

/**
 * @brief Acquire the LVGL mutex, waiting indefinitely if necessary.
 *
 * @note Call only after ui_manager_lvgl_init(). Keep the protected section
 *       short and always pair this call with ui_manager_lvgl_release_mutex().
 */
void ui_manager_lvgl_wait_for_mutex(void);

/**
 * @brief Release the LVGL mutex acquired by ui_manager_lvgl_wait_for_mutex().
 */
void ui_manager_lvgl_release_mutex(void);
