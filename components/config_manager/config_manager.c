/* Includes ----------------------------------------------------------------- */
#include "config_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"

#include <string.h>

#include "nvs.h"

const char *TAG = "CONFIG_MANAGER";

static SemaphoreHandle_t s_config_mutex = NULL;
static bool s_initialized = false;

#define CONFIG_MANAGER_MUTEX_TIMEOUT_MS 1000U

#define CONFIG_MANAGER_NVS_NAMESPACE "device_cfg"

#define CONFIG_MANAGER_NVS_KEY_VERSION "cfg_ver"
#define CONFIG_MANAGER_NVS_KEY_WIFI_SSID "wifi_ssid"
#define CONFIG_MANAGER_NVS_KEY_WIFI_PASS "wifi_pass"

#define CONFIG_MANAGER_CURRENT_VERSION 1U

static esp_err_t config_manager_lock(void);
static void config_manager_unlock(void);
static esp_err_t config_manager_validate_wifi_config(
    const config_manager_wifi_config_t *config);

static esp_err_t config_manager_validate_wifi_config(
    const config_manager_wifi_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        config != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid argument");

    size_t ssid_len = strnlen(
        config->ssid,
        CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE);

    if (ssid_len == CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (ssid_len == 0U ||
        ssid_len > CONFIG_MANAGER_WIFI_SSID_MAX_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t password_len = strnlen(
        config->password,
        CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE);

    if (password_len == CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Password rỗng được chấp nhận cho mạng open.
     * Password khác rỗng phải nằm trong khoảng 8–63 ký tự.
     */
    if (password_len != 0U &&
        (password_len < 8U ||
         password_len > CONFIG_MANAGER_WIFI_PASSWORD_MAX_LEN))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t config_manager_lock(void)
{
    if (!s_initialized || s_config_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t result = xSemaphoreTake(
        s_config_mutex,
        pdMS_TO_TICKS(CONFIG_MANAGER_MUTEX_TIMEOUT_MS));

    if (result != pdTRUE)
    {
        ESP_LOGE(TAG, "TimeOut");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void config_manager_unlock(void)
{
    if (!s_initialized || s_config_mutex == NULL)
    {
        ESP_LOGE(TAG, "invalid arguement");
        return;
    }
    else
    {
        (void)xSemaphoreGive(s_config_mutex);
    }
}

/* Functions ---------------------------------------------------------------- */
esp_err_t config_manager_init(void)
{
    if (s_initialized == true)
    {
        ESP_LOGW(TAG, "Config manager is initialized");
        return ESP_OK;
    }

    s_config_mutex = xSemaphoreCreateMutex();

    ESP_RETURN_ON_FALSE(s_config_mutex != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "Fail to create a mutex");

    s_initialized = true;

    ESP_LOGI(TAG, "Config manager is initialized");

    return ESP_OK;
}

esp_err_t config_manager_save_wifi(
    const config_manager_wifi_config_t *config)
{
    esp_err_t err = ESP_OK;

    config_manager_wifi_config_t snapshot = {0};

    nvs_handle_t handle = 0;

    bool mutex_locked = false;
    bool handle_opened = false;

    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(
        &snapshot,
        config,
        sizeof(snapshot));

    err =
        config_manager_validate_wifi_config(&snapshot);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    /* Take the component mutex before accessing NVS. */
    err = config_manager_lock();

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    mutex_locked = true;

    err = nvs_open(
        CONFIG_MANAGER_NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    handle_opened = true;

    err = nvs_set_u32(
        handle,
        CONFIG_MANAGER_NVS_KEY_VERSION,
        CONFIG_MANAGER_CURRENT_VERSION);
    if (err != ESP_OK)
    {
        goto cleanup;
    }
    err = nvs_set_str(
        handle,
        CONFIG_MANAGER_NVS_KEY_WIFI_SSID,
        snapshot.ssid);
    if (err != ESP_OK)
    {
        goto cleanup;
    }
    err = nvs_set_str(
        handle,
        CONFIG_MANAGER_NVS_KEY_WIFI_PASS,
        snapshot.password);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    ESP_LOGI(TAG, "Wi-Fi configuration saved");
    if (mutex_locked)
    {
        config_manager_unlock();
    }

    return ESP_OK;

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }
    ESP_LOGE(
        TAG,
        "Failed to save Wi-Fi configuration: %s",
        esp_err_to_name(err));

    memset(&snapshot, 0, sizeof(snapshot));

    return err;
}