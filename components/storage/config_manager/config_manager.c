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
#define CONFIG_MANAGER_NVS_KEY_DEVICE_ID "device_id"
#define CONFIG_MANAGER_NVS_KEY_DEVICE_NAME "device_name"

#define CONFIG_MANAGER_CURRENT_VERSION 1U
#define CONFIG_MANAGER_WRITE_IN_PROGRESS_VERSION UINT32_MAX

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
/**
 * @brief Acquire the component mutex for one complete storage operation.
 *
 * @return ESP_OK when locked, ESP_ERR_INVALID_STATE before initialization, or
 *         ESP_ERR_TIMEOUT when the mutex cannot be acquired in time.
 */
static esp_err_t config_manager_lock(void);

/**
 * @brief Release the component mutex after NVS handles have been closed.
 */
static void config_manager_unlock(void);

/**
 * @brief Overwrite a temporary buffer that may contain credentials.
 *
 * @param[in,out] buffer Non-NULL buffer to clear.
 * @param[in] size Number of bytes to overwrite.
 */
static void config_manager_zeroize(
    void *buffer,
    size_t size);

/**
 * @brief Validate the public Wi-Fi configuration representation.
 *
 * @param[in] config Configuration to validate.
 * @return ESP_OK when valid or ESP_ERR_INVALID_ARG when malformed.
 */
static esp_err_t config_manager_validate_wifi_config(
    const config_manager_wifi_config_t *config);

/**
 * @brief Validate the public device identity representation.
 *
 * @param[in] identity Identity to validate.
 * @return ESP_OK when valid or ESP_ERR_INVALID_ARG when malformed.
 */
static esp_err_t config_manager_validate_device_identity(
    const config_manager_device_identity_t *identity);

/**
 * @brief Check whether an NVS key exists with the expected data type.
 *
 * The caller owns the component mutex and the open NVS handle. A missing key
 * is reported as ESP_OK with @p present set to false. An existing key with a
 * different type is reported as ESP_ERR_NVS_TYPE_MISMATCH.
 *
 * @param[in] handle Open NVS handle containing the key.
 * @param[in] key Key name to inspect.
 * @param[in] expected_type Required NVS data type.
 * @param[out] present True only when the key exists with the expected type.
 * @return ESP_OK, ESP_ERR_NVS_TYPE_MISMATCH, or an NVS access error.
 */
static esp_err_t config_manager_find_expected_key(
    nvs_handle_t handle,
    const char *key,
    nvs_type_t expected_type,
    bool *present);

/**
 * @brief Map a public custom-data type to its ESP-IDF NVS type.
 *
 * @param[in] type Public custom-data type.
 * @param[out] nvs_type Matching NVS representation.
 * @return ESP_OK or ESP_ERR_INVALID_ARG for an unsupported type.
 */
static esp_err_t config_manager_data_type_to_nvs_type(
    config_manager_data_type_t type,
    nvs_type_t *nvs_type);

/**
 * @brief Read and classify Wi-Fi keys from an already-open NVS handle.
 *
 * This helper performs no locking, handle management, or commit. Type and
 * length corruption are reported through @p state; storage access failures
 * are returned directly.
 *
 * @param[in] handle Open read-only or read-write `device_cfg` handle.
 * @param[out] snapshot Cleared credential snapshot populated from NVS.
 * @param[out] state Semantic integrity state for the stored configuration.
 * @return ESP_OK when classification completed, or an NVS access error.
 */
static esp_err_t config_manager_inspect_wifi_config(
    nvs_handle_t handle,
    config_manager_wifi_config_t *snapshot,
    config_manager_wifi_config_state_t *state);

/**
 * @brief Read and validate identity from an already-open NVS handle.
 *
 * The caller owns locking and handle lifecycle. The snapshot is cleared before
 * inspection and populated only with complete stored values.
 *
 * @param[in] handle Open `device_cfg` handle.
 * @param[out] snapshot Cleared identity snapshot populated from NVS.
 * @return ESP_OK for a complete valid identity, ESP_ERR_NVS_NOT_FOUND when
 *         both keys are absent, ESP_ERR_INVALID_STATE when only one key exists,
 *         ESP_ERR_INVALID_RESPONSE for corrupt data, or an NVS access error.
 */
static esp_err_t config_manager_inspect_device_identity(
    nvs_handle_t handle,
    config_manager_device_identity_t *snapshot);

/**
 * @brief Convert a completed Wi-Fi integrity state into an API error.
 *
 * @param[in] state Classified Wi-Fi state.
 * @return Error mapping shared by load and migration APIs.
 */
static esp_err_t config_manager_wifi_state_to_error(
    config_manager_wifi_config_state_t state);

/**
 * @brief Erase and commit one component-owned namespace.
 *
 * The caller must hold the component mutex. Missing namespaces are treated as
 * already erased. Every handle opened here is closed before return.
 *
 * @param[in] namespace_name Namespace owned by config_manager.
 * @param[out] changed True when an existing namespace was erased and committed.
 * @return ESP_OK or an NVS lifecycle, erase, or commit error.
 */
static esp_err_t config_manager_erase_namespace(
    const char *namespace_name,
    bool *changed);

/* Static Functions --------------------------------------------------------- */
static esp_err_t config_manager_find_expected_key(
    nvs_handle_t handle,
    const char *key,
    nvs_type_t expected_type,
    bool *present)
{
    nvs_type_t stored_type = NVS_TYPE_ANY;

    *present = false;

    esp_err_t err = nvs_find_key(
        handle,
        key,
        &stored_type);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    if (stored_type != expected_type)
    {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    *present = true;

    return ESP_OK;
}

static esp_err_t config_manager_data_type_to_nvs_type(
    config_manager_data_type_t type,
    nvs_type_t *nvs_type)
{
    switch (type)
    {
        case CONFIG_MANAGER_DATA_TYPE_U8:
            *nvs_type = NVS_TYPE_U8;
            break;

        case CONFIG_MANAGER_DATA_TYPE_I8:
            *nvs_type = NVS_TYPE_I8;
            break;

        case CONFIG_MANAGER_DATA_TYPE_U16:
            *nvs_type = NVS_TYPE_U16;
            break;

        case CONFIG_MANAGER_DATA_TYPE_I16:
            *nvs_type = NVS_TYPE_I16;
            break;

        case CONFIG_MANAGER_DATA_TYPE_U32:
            *nvs_type = NVS_TYPE_U32;
            break;

        case CONFIG_MANAGER_DATA_TYPE_I32:
            *nvs_type = NVS_TYPE_I32;
            break;

        case CONFIG_MANAGER_DATA_TYPE_U64:
            *nvs_type = NVS_TYPE_U64;
            break;

        case CONFIG_MANAGER_DATA_TYPE_I64:
            *nvs_type = NVS_TYPE_I64;
            break;

        case CONFIG_MANAGER_DATA_TYPE_STRING:
            *nvs_type = NVS_TYPE_STR;
            break;

        case CONFIG_MANAGER_DATA_TYPE_BLOB:
            *nvs_type = NVS_TYPE_BLOB;
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t config_manager_inspect_wifi_config(
    nvs_handle_t handle,
    config_manager_wifi_config_t *snapshot,
    config_manager_wifi_config_state_t *state)
{
    bool ssid_present = false;
    bool password_present = false;

    memset(snapshot, 0, sizeof(*snapshot));
    *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t err = config_manager_find_expected_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_WIFI_SSID,
        NVS_TYPE_STR,
        &ssid_present);

    if (err == ESP_ERR_NVS_TYPE_MISMATCH)
    {
        *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    if (ssid_present)
    {
        size_t ssid_size = sizeof(snapshot->ssid);

        err = nvs_get_str(
            handle,
            CONFIG_MANAGER_NVS_KEY_WIFI_SSID,
            snapshot->ssid,
            &ssid_size);

        if (err == ESP_ERR_NVS_INVALID_LENGTH)
        {
            *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
            return ESP_OK;
        }

        if (err != ESP_OK)
        {
            return err;
        }
    }

    err = config_manager_find_expected_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_WIFI_PASS,
        NVS_TYPE_STR,
        &password_present);

    if (err == ESP_ERR_NVS_TYPE_MISMATCH)
    {
        *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    if (password_present)
    {
        size_t password_size = sizeof(snapshot->password);

        err = nvs_get_str(
            handle,
            CONFIG_MANAGER_NVS_KEY_WIFI_PASS,
            snapshot->password,
            &password_size);

        if (err == ESP_ERR_NVS_INVALID_LENGTH)
        {
            *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
            return ESP_OK;
        }

        if (err != ESP_OK)
        {
            return err;
        }
    }

    const bool any_credential_present =
        ssid_present ||
        password_present;

    if (!any_credential_present)
    {
        *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED;
        return ESP_OK;
    }

    if (!ssid_present ||
        !password_present)
    {
        *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE;
        return ESP_OK;
    }

    err = config_manager_validate_wifi_config(snapshot);

    if (err != ESP_OK)
    {
        *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
        return ESP_OK;
    }

    bool version_present = false;

    err = config_manager_find_expected_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_VERSION,
        NVS_TYPE_U32,
        &version_present);

    if (err == ESP_ERR_NVS_TYPE_MISMATCH)
    {
        *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA;
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    if (!version_present)
    {
        *state =
            CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED;
        return ESP_OK;
    }

    uint32_t stored_version = 0U;

    err = nvs_get_u32(
        handle,
        CONFIG_MANAGER_NVS_KEY_VERSION,
        &stored_version);

    if (err != ESP_OK)
    {
        return err;
    }

    if (stored_version == 0U)
    {
        *state =
            CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED;
    }
    else if (stored_version == CONFIG_MANAGER_CURRENT_VERSION)
    {
        *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID;
    }
    else
    {
        *state =
            CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION;
    }

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

static esp_err_t config_manager_validate_device_identity(
    const config_manager_device_identity_t *identity)
{
    ESP_RETURN_ON_FALSE(
        identity != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid argument");

    const size_t device_id_len = strnlen(
        identity->device_id,
        CONFIG_MANAGER_DEVICE_ID_BUFFER_SIZE);

    if (device_id_len == 0U ||
        device_id_len == CONFIG_MANAGER_DEVICE_ID_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t device_name_len = strnlen(
        identity->device_name,
        CONFIG_MANAGER_DEVICE_NAME_BUFFER_SIZE);

    if (device_name_len == 0U ||
        device_name_len == CONFIG_MANAGER_DEVICE_NAME_BUFFER_SIZE)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t config_manager_inspect_device_identity(
    nvs_handle_t handle,
    config_manager_device_identity_t *snapshot)
{
    bool device_id_present = false;
    bool device_name_present = false;

    memset(snapshot, 0, sizeof(*snapshot));

    esp_err_t err = config_manager_find_expected_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_DEVICE_ID,
        NVS_TYPE_STR,
        &device_id_present);

    if (err == ESP_ERR_NVS_TYPE_MISMATCH)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    err = config_manager_find_expected_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_DEVICE_NAME,
        NVS_TYPE_STR,
        &device_name_present);

    if (err == ESP_ERR_NVS_TYPE_MISMATCH)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    if (!device_id_present &&
        !device_name_present)
    {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    if (!device_id_present ||
        !device_name_present)
    {
        return ESP_ERR_INVALID_STATE;
    }

    size_t device_id_size = sizeof(snapshot->device_id);

    err = nvs_get_str(
        handle,
        CONFIG_MANAGER_NVS_KEY_DEVICE_ID,
        snapshot->device_id,
        &device_id_size);

    if (err == ESP_ERR_NVS_INVALID_LENGTH)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    size_t device_name_size = sizeof(snapshot->device_name);

    err = nvs_get_str(
        handle,
        CONFIG_MANAGER_NVS_KEY_DEVICE_NAME,
        snapshot->device_name,
        &device_name_size);

    if (err == ESP_ERR_NVS_INVALID_LENGTH)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    err = config_manager_validate_device_identity(snapshot);

    if (err != ESP_OK)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t config_manager_wifi_state_to_error(
    config_manager_wifi_config_state_t state)
{
    switch (state)
    {
        case CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID:
            return ESP_OK;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED:
            return ESP_ERR_NVS_NOT_FOUND;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE:
        case CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED:
            return ESP_ERR_INVALID_STATE;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION:
            return ESP_ERR_NOT_SUPPORTED;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA:
            return ESP_ERR_INVALID_RESPONSE;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN:
        default:
            return ESP_ERR_INVALID_STATE;
    }
}

static esp_err_t config_manager_erase_namespace(
    const char *namespace_name,
    bool *changed)
{
    nvs_handle_t handle = 0;

    *changed = false;

    esp_err_t err = nvs_open(
        namespace_name,
        NVS_READONLY,
        &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    nvs_close(handle);
    handle = 0;

    err = nvs_open(
        namespace_name,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_erase_all(handle);

    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK)
    {
        *changed = true;
    }

    return err;
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
        ESP_LOGD(TAG, "Config manager is already initialized");
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
    config_manager_wifi_config_t stored_snapshot = {0};
    config_manager_wifi_config_state_t stored_state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    nvs_handle_t handle = 0;

    bool mutex_locked = false;
    bool handle_opened = false;
    bool config_changed = false;

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

    err = config_manager_inspect_wifi_config(
        handle,
        &stored_snapshot,
        &stored_state);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    if (stored_state == CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID &&
        strcmp(snapshot.ssid, stored_snapshot.ssid) == 0 &&
        strcmp(snapshot.password, stored_snapshot.password) == 0)
    {
        goto cleanup;
    }

    /*
     * Persist a fail-closed marker before changing either credential key.
     * A reset or flash error during the following multi-key update therefore
     * leaves an unsupported schema instead of a potentially mixed credential
     * pair that boot could mistake for a valid configuration.
     */
    err = nvs_set_u32(
        handle,
        CONFIG_MANAGER_NVS_KEY_VERSION,
        CONFIG_MANAGER_WRITE_IN_PROGRESS_VERSION);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);

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

    /*
     * Keep the fail-closed marker durable while the credential pair becomes
     * durable. The current version is published only after both values commit.
     */
    err = nvs_commit(handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_u32(
        handle,
        CONFIG_MANAGER_NVS_KEY_VERSION,
        CONFIG_MANAGER_CURRENT_VERSION);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    config_changed = true;

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
    config_manager_zeroize(
        &stored_snapshot,
        sizeof(stored_snapshot));

    if (err == ESP_OK)
    {
        if (config_changed)
        {
            ESP_LOGI(TAG, "Wi-Fi configuration saved");
        }
        else
        {
            ESP_LOGD(TAG, "Wi-Fi configuration is already current");
        }
    }
    else
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

    if (state == CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID)
    {
        memcpy(
            config,
            &snapshot,
            sizeof(*config));
    }

    err = config_manager_wifi_state_to_error(state);

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
    config_manager_zeroize(
        &snapshot,
        sizeof(snapshot));

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

esp_err_t config_manager_migrate_device_config(void)
{
    esp_err_t err = ESP_OK;

    config_manager_wifi_config_t snapshot = {0};
    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

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

    err = config_manager_inspect_wifi_config(
        handle,
        &snapshot,
        &state);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    if (state == CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED)
    {
        err = nvs_set_u32(
            handle,
            CONFIG_MANAGER_NVS_KEY_VERSION,
            CONFIG_MANAGER_CURRENT_VERSION);

        if (err != ESP_OK)
        {
            goto cleanup;
        }

        err = nvs_commit(handle);

        if (err != ESP_OK)
        {
            goto cleanup;
        }

        config_changed = true;
    }
    else
    {
        err = config_manager_wifi_state_to_error(state);
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

    config_manager_zeroize(
        &snapshot,
        sizeof(snapshot));

    if (err == ESP_OK)
    {
        if (config_changed)
        {
            ESP_LOGI(TAG, "Wi-Fi configuration migrated to schema version 1");
        }
        else
        {
            ESP_LOGD(TAG, "Wi-Fi configuration already uses current schema");
        }
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGD(TAG, "No Wi-Fi configuration is available to migrate");
    }
    else
    {
        ESP_LOGW(
            TAG,
            "Failed to migrate Wi-Fi configuration: state=%d, error=%s",
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

esp_err_t config_manager_save_device_identity(
    const config_manager_device_identity_t *identity)
{
    ESP_RETURN_ON_FALSE(
        identity != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid argument");

    esp_err_t err = ESP_OK;

    config_manager_device_identity_t snapshot = {0};
    nvs_handle_t handle = 0;
    bool mutex_locked = false;
    bool handle_opened = false;

    memcpy(
        &snapshot,
        identity,
        sizeof(snapshot));

    err = config_manager_validate_device_identity(&snapshot);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

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

    err = nvs_set_str(
        handle,
        CONFIG_MANAGER_NVS_KEY_DEVICE_ID,
        snapshot.device_id);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_set_str(
        handle,
        CONFIG_MANAGER_NVS_KEY_DEVICE_NAME,
        snapshot.device_name);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = nvs_commit(handle);

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

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Device identity saved");
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to save device identity: %s",
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_load_device_identity(
    config_manager_device_identity_t *identity)
{
    if (identity == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(identity, 0, sizeof(*identity));

    esp_err_t err = ESP_OK;

    config_manager_device_identity_t snapshot = {0};
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

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    handle_opened = true;

    err = config_manager_inspect_device_identity(
        handle,
        &snapshot);

    if (err == ESP_OK)
    {
        memcpy(
            identity,
            &snapshot,
            sizeof(*identity));
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

    config_manager_zeroize(
        &snapshot,
        sizeof(snapshot));

    if (err != ESP_OK)
    {
        memset(identity, 0, sizeof(*identity));
    }

    if (err == ESP_OK)
    {
        ESP_LOGD(TAG, "Device identity loaded");
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGD(TAG, "Device identity is not stored");
    }
    else
    {
        ESP_LOGW(
            TAG,
            "Failed to load device identity: %s",
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_clear_device_identity(void)
{
    esp_err_t err = ESP_OK;
    nvs_handle_t handle = 0;
    bool mutex_locked = false;
    bool handle_opened = false;
    bool identity_changed = false;

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
        CONFIG_MANAGER_NVS_KEY_DEVICE_ID);

    if (err == ESP_OK)
    {
        identity_changed = true;
    }
    else if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        goto cleanup;
    }

    err = nvs_erase_key(
        handle,
        CONFIG_MANAGER_NVS_KEY_DEVICE_NAME);

    if (err == ESP_OK)
    {
        identity_changed = true;
    }
    else if (err != ESP_ERR_NVS_NOT_FOUND)
    {
        goto cleanup;
    }

    err = ESP_OK;

    if (identity_changed)
    {
        err = nvs_commit(handle);
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
        if (identity_changed)
        {
            ESP_LOGI(TAG, "Device identity cleared");
        }
        else
        {
            ESP_LOGD(TAG, "Device identity was already clear");
        }
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to clear device identity: %s",
            esp_err_to_name(err));
    }

    return err;
}

esp_err_t config_manager_has_device_identity(
    bool *has_identity)
{
    if (has_identity == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *has_identity = false;

    config_manager_device_identity_t identity = {0};

    esp_err_t err =
        config_manager_load_device_identity(&identity);

    if (err == ESP_OK)
    {
        *has_identity = true;
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = ESP_OK;
    }

    config_manager_zeroize(
        &identity,
        sizeof(identity));

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
    nvs_type_t expected_nvs_type = NVS_TYPE_ANY;

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

    err = config_manager_data_type_to_nvs_type(
        type,
        &expected_nvs_type);

    if (err != ESP_OK)
    {
        return err;
    }

    if (out_value != NULL)
    {
        memset(out_value, 0, *inout_size);
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

    bool key_present = false;

    err = config_manager_find_expected_key(
        handle,
        key,
        expected_nvs_type,
        &key_present);

    if (err != ESP_OK)
    {
        goto cleanup;
    }

    if (!key_present)
    {
        err = ESP_ERR_NVS_NOT_FOUND;
        goto cleanup;
    }

    /*
     * Read the value using the matching NVS getter.
     *
     * Integer values are first read into a local variable and copied
     * to the caller only after nvs_get_*() succeeds. The caller's
     * validated output buffer was cleared before NVS access.
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
        /* A missing namespace means Wi-Fi has not been configured. */
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

    config_manager_zeroize(
        &snapshot,
        sizeof(snapshot));

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

esp_err_t config_manager_factory_reset(void)
{
    esp_err_t err = config_manager_lock();

    if (err != ESP_OK)
    {
        return err;
    }

    bool device_config_changed = false;
    bool custom_config_changed = false;

    const esp_err_t device_config_err = config_manager_erase_namespace(
        CONFIG_MANAGER_NVS_NAMESPACE,
        &device_config_changed);

    /*
     * Always attempt both independently owned namespaces. This preserves the
     * best possible reset state even if the first namespace reports an error.
     */
    const esp_err_t custom_config_err = config_manager_erase_namespace(
        CONFIG_MANAGER_CUSTOM_NVS_NAMESPACE,
        &custom_config_changed);

    err = device_config_err != ESP_OK
        ? device_config_err
        : custom_config_err;

    config_manager_unlock();

    if (err == ESP_OK)
    {
        if (device_config_changed ||
            custom_config_changed)
        {
            ESP_LOGI(TAG, "Component-owned configuration reset");
        }
        else
        {
            ESP_LOGD(TAG, "Component-owned configuration was already reset");
        }
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Component reset incomplete: device_cfg=%s, custom_cfg=%s",
            esp_err_to_name(device_config_err),
            esp_err_to_name(custom_config_err));
    }

    return err;
}
