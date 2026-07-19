#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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
typedef enum
{
    CONFIG_MANAGER_DATA_TYPE_U8 = 0,
    CONFIG_MANAGER_DATA_TYPE_I8,
    CONFIG_MANAGER_DATA_TYPE_U16,
    CONFIG_MANAGER_DATA_TYPE_I16,
    CONFIG_MANAGER_DATA_TYPE_U32,
    CONFIG_MANAGER_DATA_TYPE_I32,
    CONFIG_MANAGER_DATA_TYPE_U64,
    CONFIG_MANAGER_DATA_TYPE_I64,
    CONFIG_MANAGER_DATA_TYPE_STRING,
    CONFIG_MANAGER_DATA_TYPE_BLOB,
} config_manager_data_type_t;

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

    esp_err_t config_manager_has_wifi_config(
    bool *has_config);

esp_err_t config_manager_save_custom_data(
    const char *key,
    const void *value,
    size_t value_size,
    config_manager_data_type_t type);

esp_err_t config_manager_load_custom_data(
    const char *key,
    void *out_value,
    size_t *inout_size,
    config_manager_data_type_t type);

esp_err_t config_manager_clear_custom_data(
    const char *key);
#ifdef __cplusplus
}
#endif
