/* Includes ----------------------------------------------------------------- */
#include "config_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "nvs.h"

/* Macros ------------------------------------------------------------------- */
#define CONFIG_MANAGER_MUTEX_TIMEOUT_MS 1000U

#define CONFIG_MANAGER_NVS_NAMESPACE "device_cfg"

#define CONFIG_MANAGER_NVS_KEY_VERSION "cfg_ver"
#define CONFIG_MANAGER_NVS_KEY_WIFI_SSID "wifi_ssid"
#define CONFIG_MANAGER_NVS_KEY_WIFI_PASS "wifi_pass"

#define CONFIG_MANAGER_CURRENT_VERSION 1U

#define CONFIG_MANAGER_CUSTOM_NVS_NAMESPACE "custom_cfg"

#define CONFIG_MANAGER_NVS_KEY_BUFFER_SIZE \
    (CONFIG_MANAGER_CUSTOM_KEY_MAX_LEN + 1U)

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "CONFIG_MANAGER";

/* Static Variables --------------------------------------------------------- */
static portMUX_TYPE s_init_lock =
    portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_config_mutex = NULL;
static bool s_initialized = false;
static bool s_initializing = false;

/* Function Prototypes ------------------------------------------------------ */
static esp_err_t config_manager_lock(void);

static void config_manager_unlock(void);

static void config_manager_zeroize(
    void *buffer,
    size_t size);

static esp_err_t config_manager_validate_wifi_config(
    const config_manager_wifi_config_t *config);

static esp_err_t config_manager_inspect_wifi_config(
    nvs_handle_t handle,
    config_manager_wifi_config_t *snapshot,
    config_manager_wifi_config_state_t *state);

/* Static Functions --------------------------------------------------------- */
static esp_err_t config_manager_inspect_wifi_config(
    nvs_handle_t handle,
    config_manager_wifi_config_t *snapshot,
    config_manager_wifi_config_state_t *state)
{
bool version_present = false;
bool ssid_present = false;
bool password_present = false;

memset(snapshot, 0, sizeof(*snapshot));
*state = CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

uint32_t stored_version = 0U;

esp_err_t err = nvs_get_u32(
    handle,
    CONFIG_MANAGER_NVS_KEY_VERSION,
    &stored_version);

if (err == ESP_OK)
{
    version_present = true;
}
else if (err == ESP_ERR_NVS_NOT_FOUND)
{
    /* Version key không tồn tại. */
}
else if (err == ESP_ERR_NVS_TYPE_MISMATCH)
{
    *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
    return ESP_OK;
}
else
{
    return err;
}

size_t ssid_size = sizeof(snapshot->ssid);

err = nvs_get_str(
    handle,
    CONFIG_MANAGER_NVS_KEY_WIFI_SSID,
    snapshot->ssid,
    &ssid_size);

if (err == ESP_OK)
{
    ssid_present = true;
}
else if (err == ESP_ERR_NVS_NOT_FOUND)
{
    /* The SSID key is not stored. */
}
else if (err == ESP_ERR_NVS_TYPE_MISMATCH ||
         err == ESP_ERR_NVS_INVALID_LENGTH)
{
    *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
    return ESP_OK;
}
else
{
    return err;
}

size_t password_size = sizeof(snapshot->password);

err = nvs_get_str(
    handle,
    CONFIG_MANAGER_NVS_KEY_WIFI_PASS,
    snapshot->password,
    &password_size);

if (err == ESP_OK)
{
    password_present = true;
}
else if (err == ESP_ERR_NVS_NOT_FOUND)
{
    /* Password key không tồn tại. */
}
else if (err == ESP_ERR_NVS_TYPE_MISMATCH ||
         err == ESP_ERR_NVS_INVALID_LENGTH)
{
    *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
    return ESP_OK;
}
else
{
    return err;
}

const bool any_key_present =
    version_present ||
    ssid_present ||
    password_present;

const bool all_keys_present =
    version_present &&
    ssid_present &&
    password_present;

if (!any_key_present)
{
    *state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED;

    return ESP_OK;
}

if (!all_keys_present)
{
    *state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE;

    return ESP_OK;
}

if (stored_version != CONFIG_MANAGER_CURRENT_VERSION)
{
    *state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION;

    return ESP_OK;
}

err = config_manager_validate_wifi_config(snapshot);

if (err != ESP_OK)
{
    *state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;

    return ESP_OK;
}

*state = CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID;

return ESP_OK;

}

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

    /* Open networks use an empty password; secured passwords use 8-63 bytes. */
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
    SemaphoreHandle_t mutex = NULL;

    taskENTER_CRITICAL(&s_init_lock);

    if (s_initialized)
    {
        mutex = s_config_mutex;
    }

    taskEXIT_CRITICAL(&s_init_lock);

    if (mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t result = xSemaphoreTake(
        mutex,
        pdMS_TO_TICKS(CONFIG_MANAGER_MUTEX_TIMEOUT_MS));

    if (result != pdTRUE)
    {
        ESP_LOGE(TAG, "Timed out waiting for configuration mutex");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void config_manager_unlock(void)
{
    SemaphoreHandle_t mutex = NULL;

    taskENTER_CRITICAL(&s_init_lock);
    mutex = s_config_mutex;
    taskEXIT_CRITICAL(&s_init_lock);

    if (mutex == NULL)
    {
        ESP_LOGE(TAG, "Configuration mutex is unavailable");
        return;
    }

    if (xSemaphoreGive(mutex) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to release configuration mutex");
    }
}

static void config_manager_zeroize(
    void *buffer,
    size_t size)
{
    /* Volatile writes prevent removal of credential cleanup by optimization. */
    volatile uint8_t *bytes =
        (volatile uint8_t *)buffer;

    while (size > 0U)
    {
        *bytes++ = 0U;
        size--;
    }
}

/* Functions ---------------------------------------------------------------- */
esp_err_t config_manager_init(void)
{
    taskENTER_CRITICAL(&s_init_lock);

    if (s_initialized)
    {
        taskEXIT_CRITICAL(&s_init_lock);
        ESP_LOGW(TAG, "Config manager is initialized");
        return ESP_OK;
    }

    if (s_initializing)
    {
        taskEXIT_CRITICAL(&s_init_lock);
        ESP_LOGW(TAG, "Config manager initialization is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_initializing = true;
    taskEXIT_CRITICAL(&s_init_lock);

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();

    if (mutex == NULL)
    {
        taskENTER_CRITICAL(&s_init_lock);
        s_initializing = false;
        taskEXIT_CRITICAL(&s_init_lock);

        ESP_LOGE(TAG, "Failed to create configuration mutex");
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_init_lock);
    s_config_mutex = mutex;
    s_initialized = true;
    s_initializing = false;
    taskEXIT_CRITICAL(&s_init_lock);

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

    ESP_RETURN_ON_FALSE(
        config != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid argument");

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

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }

    config_manager_zeroize(
        &snapshot,
        sizeof(snapshot));

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to save Wi-Fi configuration: %s",
            esp_err_to_name(err));
    }

    return err;
}
esp_err_t config_manager_load_wifi(
    config_manager_wifi_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));

    esp_err_t err = ESP_OK;

    config_manager_wifi_config_t snapshot = {0};

    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    nvs_handle_t handle = 0;

    bool mutex_locked = false;
    bool handle_opened = false;

    /*
     * Protect the complete read transaction.
     */
    err = config_manager_lock();

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    mutex_locked = true;

    /*
     * Open the Wi-Fi/device configuration namespace.
     */
    err = nvs_open(
        CONFIG_MANAGER_NVS_NAMESPACE,
        NVS_READONLY,
        &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        /*
         * Namespace does not exist yet:
         * Wi-Fi configuration has never been saved.
         */
        err = ESP_ERR_NVS_NOT_FOUND;
        goto cleanup;
    }

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    handle_opened = true;

    /*
     * Read and classify the Wi-Fi configuration.
     *
     * This helper does not lock or open/close NVS.
     */
    err = config_manager_inspect_wifi_config(
        handle,
        &snapshot,
        &state);

    if (err != ESP_OK)
    {
        /*
         * Actual storage/access failure.
         */
        goto cleanup;
    }

    /*
     * Convert the semantic state into the public load result.
     */
    switch (state)
    {
        case CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID:
            memcpy(
                config,
                &snapshot,
                sizeof(*config));

            err = ESP_OK;
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED:
            err = ESP_ERR_NVS_NOT_FOUND;
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE:
            err = ESP_ERR_INVALID_STATE;
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION:
            err = ESP_ERR_NOT_SUPPORTED;
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA:
            err = ESP_ERR_INVALID_RESPONSE;
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN:
        default:
            err = ESP_ERR_INVALID_STATE;
            break;
    }

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }

    /*
     * Remove the local credential copy.
     */
    memset(&snapshot, 0, sizeof(snapshot));

    /*
     * Never return partial or stale credentials on failure.
     */
    if (err != ESP_OK)
    {
        memset(config, 0, sizeof(*config));
    }

    if (err == ESP_OK)
    {
        ESP_LOGD(TAG, "Wi-Fi configuration loaded");
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGD(TAG, "Wi-Fi configuration is not stored");
    }
    else
    {
        ESP_LOGW(
            TAG,
            "Failed to load Wi-Fi configuration: state=%d, error=%s",
            (int)state,
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_clear_wifi(void)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t handle = 0;

    bool mutex_locked = false;
    bool handle_opened = false;
    bool config_changed = false;

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

    err = nvs_erase_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_WIFI_SSID);

    if (err == ESP_OK)
    {
        config_changed = true;
    }
    else if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        goto cleanup;
    }

    err = nvs_erase_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_WIFI_PASS);

    if (err == ESP_OK)
    {
        config_changed = true;
    }
    else if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        goto cleanup;
    }

    /* Missing keys already satisfy the requested cleared state. */
    err = ESP_OK;

    if (config_changed)
    {
        err = nvs_commit(handle);

        if (err != ESP_OK)
        {
            goto cleanup;
        }
    }

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }

    if (err == ESP_OK)
    {
        if (config_changed)
        {
            ESP_LOGI(TAG, "Wi-Fi configuration cleared");
        }
        else
        {
            ESP_LOGD(TAG, "Wi-Fi configuration was already clear");
        }
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to clear Wi-Fi configuration: %s",
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_has_wifi_config(
    bool *has_config)
{
    if (has_config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *has_config = false;

    config_manager_wifi_config_t config = {0};

    esp_err_t err = config_manager_load_wifi(&config);

    if (err == ESP_OK)
    {
        *has_config = true;
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }

    config_manager_zeroize(
        &config,
        sizeof(config));

    if (err == ESP_OK)
    {
        ESP_LOGD(
            TAG,
            "Wi-Fi configuration presence checked: present=%s",
            *has_config ? "true" : "false");
    }
    else
    {
        ESP_LOGW(
            TAG,
            "Failed to check Wi-Fi configuration presence: %s",
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_load_custom_data(
    const char *key,
    void *out_value,
    size_t *inout_size,
    config_manager_data_type_t type)
{
    esp_err_t err = ESP_OK;

    nvs_handle_t handle = 0;
    bool mutex_locked = false;
    bool handle_opened = false;

    /*
     * Validate common arguments.
     */
    if (key == NULL || inout_size == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t key_len = strnlen(
        key,
        CONFIG_MANAGER_NVS_KEY_BUFFER_SIZE);

    if (key_len == 0U ||
        key_len == CONFIG_MANAGER_NVS_KEY_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Validate output buffer size for fixed-size integer types.
     *
     * STRING and BLOB permit out_value == NULL so that the caller
     * can query the required buffer size.
     */
    switch (type)
    {
        case CONFIG_MANAGER_DATA_TYPE_U8:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(uint8_t))
            {
                *inout_size = sizeof(uint8_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I8:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(int8_t))
            {
                *inout_size = sizeof(int8_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_U16:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(uint16_t))
            {
                *inout_size = sizeof(uint16_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I16:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(int16_t))
            {
                *inout_size = sizeof(int16_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_U32:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(uint32_t))
            {
                *inout_size = sizeof(uint32_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I32:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(int32_t))
            {
                *inout_size = sizeof(int32_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_U64:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(uint64_t))
            {
                *inout_size = sizeof(uint64_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I64:
            if (out_value == NULL)
            {
                return ESP_ERR_INVALID_ARG;
            }

            if (*inout_size != sizeof(int64_t))
            {
                *inout_size = sizeof(int64_t);
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_STRING:
        case CONFIG_MANAGER_DATA_TYPE_BLOB:
            /*
             * out_value == NULL:
             * query required size only.
             *
             * out_value != NULL:
             * caller must provide a non-zero buffer size.
             */
            if (out_value != NULL && *inout_size == 0U)
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    err = config_manager_lock();

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    mutex_locked = true;

    err = nvs_open(
        CONFIG_MANAGER_CUSTOM_NVS_NAMESPACE,
        NVS_READONLY,
        &handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    handle_opened = true;

    /*
     * Read the value using the matching NVS getter.
     *
     * Integer values are first read into a local variable and copied
     * to the caller only after nvs_get_*() succeeds. This keeps the
     * caller's output unchanged when the read operation fails.
     */
    switch (type)
    {
        case CONFIG_MANAGER_DATA_TYPE_U8:
        {
            uint8_t value = 0U;

            err = nvs_get_u8(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I8:
        {
            int8_t value = 0;

            err = nvs_get_i8(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_U16:
        {
            uint16_t value = 0U;

            err = nvs_get_u16(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I16:
        {
            int16_t value = 0;

            err = nvs_get_i16(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_U32:
        {
            uint32_t value = 0U;

            err = nvs_get_u32(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I32:
        {
            int32_t value = 0;

            err = nvs_get_i32(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_U64:
        {
            uint64_t value = 0U;

            err = nvs_get_u64(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I64:
        {
            int64_t value = 0;

            err = nvs_get_i64(handle, key, &value);

            if (err == ESP_OK)
            {
                memcpy(out_value, &value, sizeof(value));
                *inout_size = sizeof(value);
            }
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_STRING:
            err = nvs_get_str(
                handle,
                key,
                (char *)out_value,
                inout_size);
            break;

        case CONFIG_MANAGER_DATA_TYPE_BLOB:
            err = nvs_get_blob(
                handle,
                key,
                out_value,
                inout_size);
            break;

        default:
            /*
             * Already validated above. This is a defensive fallback.
             */
            err = ESP_ERR_INVALID_ARG;
            break;
    }

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }

    if (err == ESP_OK)
    {
        ESP_LOGD(
            TAG,
            "Custom configuration loaded: key=%s, type=%d, size=%u",
            key,
            (int)type,
            (unsigned int)*inout_size);
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGD(
            TAG,
            "Custom configuration is not stored: key=%s",
            key);
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to load custom configuration: key=%s, type=%d, error=%s",
            key,
            (int)type,
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_save_custom_data(
    const char *key,
    const void *value,
    size_t value_size,
    config_manager_data_type_t type)
{
    esp_err_t err = ESP_OK;

    nvs_handle_t handle = 0;

    bool mutex_locked = false;
    bool handle_opened = false;

    /*
     * Validate common arguments.
     */
    if (key == NULL || value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t key_len = strnlen(
        key,
        CONFIG_MANAGER_NVS_KEY_BUFFER_SIZE);

    /*
     * Empty key or key longer than the NVS limit.
     */
    if (key_len == 0U ||
        key_len == CONFIG_MANAGER_NVS_KEY_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Validate the declared value size against the selected type.
     */
    switch (type)
    {
        case CONFIG_MANAGER_DATA_TYPE_U8:
            if (value_size != sizeof(uint8_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I8:
            if (value_size != sizeof(int8_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_U16:
            if (value_size != sizeof(uint16_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I16:
            if (value_size != sizeof(int16_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_U32:
            if (value_size != sizeof(uint32_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I32:
            if (value_size != sizeof(int32_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_U64:
            if (value_size != sizeof(uint64_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_I64:
            if (value_size != sizeof(int64_t))
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        case CONFIG_MANAGER_DATA_TYPE_STRING:
        {
            if (value_size == 0U)
            {
                return ESP_ERR_INVALID_SIZE;
            }

            /*
             * Verify that the string contains a null terminator
             * within the caller-declared buffer size.
             */
            const size_t string_len = strnlen(
                (const char *)value,
                value_size);

            if (string_len == value_size)
            {
                return ESP_ERR_INVALID_ARG;
            }

            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_BLOB:
            if (value_size == 0U ||
                value_size > CONFIG_MANAGER_CUSTOM_BLOB_MAX_SIZE)
            {
                return ESP_ERR_INVALID_SIZE;
            }
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    /*
     * Serialize the complete NVS write transaction.
     */
    err = config_manager_lock();

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    mutex_locked = true;

    err = nvs_open(
        CONFIG_MANAGER_CUSTOM_NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    handle_opened = true;

    /*
     * Use memcpy for integer types instead of directly dereferencing
     * the void pointer. This also avoids unaligned pointer access.
     */
    switch (type)
    {
        case CONFIG_MANAGER_DATA_TYPE_U8:
        {
            uint8_t stored_value = 0U;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_u8(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I8:
        {
            int8_t stored_value = 0;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_i8(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_U16:
        {
            uint16_t stored_value = 0U;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_u16(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I16:
        {
            int16_t stored_value = 0;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_i16(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_U32:
        {
            uint32_t stored_value = 0U;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_u32(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I32:
        {
            int32_t stored_value = 0;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_i32(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_U64:
        {
            uint64_t stored_value = 0U;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_u64(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_I64:
        {
            int64_t stored_value = 0;

            memcpy(
                &stored_value,
                value,
                sizeof(stored_value));

            err = nvs_set_i64(
                handle,
                key,
                stored_value);
            break;
        }

        case CONFIG_MANAGER_DATA_TYPE_STRING:
            err = nvs_set_str(
                handle,
                key,
                (const char *)value);
            break;

        case CONFIG_MANAGER_DATA_TYPE_BLOB:
            err = nvs_set_blob(
                handle,
                key,
                value,
                value_size);
            break;

        default:
            /*
             * Defensive fallback. The type was already validated.
             */
            err = ESP_ERR_INVALID_ARG;
            break;
    }

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    /*
     * nvs_set_* only stages the modification.
     * Commit makes it persistent in flash.
     */
    err = nvs_commit(handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    ESP_LOGI(
        TAG,
        "Custom configuration saved: key=%s",
        key);

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to save custom configuration: key=%s, error=%s",
            key,
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_clear_custom_data(
    const char *key)
{
    esp_err_t err = ESP_OK;

    nvs_handle_t handle = 0;

    bool mutex_locked = false;
    bool handle_opened = false;
    bool data_changed = false;

    /*
     * Validate key.
     */
    if (key == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t key_len = strnlen(
        key,
        CONFIG_MANAGER_NVS_KEY_BUFFER_SIZE);

    if (key_len == 0U ||
        key_len == CONFIG_MANAGER_NVS_KEY_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Serialize the complete NVS operation.
     */
    err = config_manager_lock();

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    mutex_locked = true;

    err = nvs_open(
        CONFIG_MANAGER_CUSTOM_NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    handle_opened = true;

    err = nvs_erase_key(
        handle,
        key);

    if (err == ESP_OK)
    {
        data_changed = true;
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        /*
         * Idempotent behavior:
         * the requested key is already absent.
         */
        err = ESP_OK;
    }
    else
    {
        goto cleanup;
    }

    /*
     * Commit only when an entry was actually erased.
     */
    if (data_changed)
    {
        err = nvs_commit(handle);

        if (err != ESP_OK)
        {
            goto cleanup;
        }
    }

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }

    if (err == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Custom configuration cleared: key=%s",
            key);
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to clear custom configuration: key=%s, error=%s",
            key,
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_get_wifi_config_state(
    config_manager_wifi_config_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t err = ESP_OK;

    config_manager_wifi_config_t snapshot = {0};

    nvs_handle_t handle = 0;
    bool mutex_locked = false;
    bool handle_opened = false;

    err = config_manager_lock();

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    mutex_locked = true;

    err = nvs_open(
        CONFIG_MANAGER_NVS_NAMESPACE,
        NVS_READONLY,
        &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        /*
         * Namespace chưa tồn tại.
         * Đây là trạng thái chưa cấu hình, không phải storage failure.
         */
        *state =
            CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED;

        err = ESP_OK;
        goto cleanup;
    }

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    handle_opened = true;

    err = config_manager_inspect_wifi_config(
        handle,
        &snapshot,
        state);

cleanup:

    if (handle_opened)
    {
        nvs_close(handle);
    }

    if (mutex_locked)
    {
        config_manager_unlock();
    }

    memset(&snapshot, 0, sizeof(snapshot));

    if (err != ESP_OK)
    {
        *state =
            CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

        ESP_LOGE(
            TAG,
            "Failed to inspect Wi-Fi configuration state: %s",
            esp_err_to_name(err));
    }
    else
    {
        ESP_LOGD(
            TAG,
            "Wi-Fi configuration state inspected: state=%d",
            (int)*state);
    }

    return err;
}
