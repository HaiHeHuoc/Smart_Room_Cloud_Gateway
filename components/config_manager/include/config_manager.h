#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Macros ------------------------------------------------------------------- */
#define CONFIG_MANAGER_WIFI_SSID_MAX_LEN 32U
#define CONFIG_MANAGER_WIFI_PASSWORD_MAX_LEN 63U

#define CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE \
    (CONFIG_MANAGER_WIFI_SSID_MAX_LEN + 1U)

#define CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE \
    (CONFIG_MANAGER_WIFI_PASSWORD_MAX_LEN + 1U)

#ifdef __cplusplus
extern "C"
{
#endif

    /* Type Definitions --------------------------------------------------------- */
    typedef struct
    {
        char ssid[CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE];
        char password[CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE];
    } config_manager_wifi_config_t;

    /* Functions ---------------------------------------------------------------- */
    esp_err_t config_manager_init(void);

    esp_err_t config_manager_save_wifi(
        const config_manager_wifi_config_t *config);

    esp_err_t config_manager_load_wifi(
        config_manager_wifi_config_t *config);

    esp_err_t config_manager_clear_wifi(void);
#ifdef __cplusplus
}
#endif
