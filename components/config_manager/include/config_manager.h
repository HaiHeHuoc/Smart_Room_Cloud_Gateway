#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Macros ------------------------------------------------------------------- */
#define CONFIG_MANAGER_WIFI_SSID_MAX_LEN       32U
#define CONFIG_MANAGER_WIFI_PASSWORD_MAX_LEN   63U

#define CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE \
    (CONFIG_MANAGER_WIFI_SSID_MAX_LEN + 1U)

#define CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE \
    (CONFIG_MANAGER_WIFI_PASSWORD_MAX_LEN + 1U)

/** Maximum custom NVS key length, excluding the null terminator. */
#define CONFIG_MANAGER_CUSTOM_KEY_MAX_LEN      15U

/** Maximum blob accepted by config_manager_save_custom_data(). */
#define CONFIG_MANAGER_CUSTOM_BLOB_MAX_SIZE    512U

#ifdef __cplusplus
extern "C" {
#endif

/* Type Definitions --------------------------------------------------------- */
/** @brief NVS value types supported by the generic custom-data APIs. */
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

typedef enum
{
    CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN = 0,

    CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED,

    CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID,

    CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE,

    CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION,

    CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA,
} config_manager_wifi_config_state_t;

/** @brief Wi-Fi credentials copied to or from the `device_cfg` namespace. */
typedef struct
{
    /** Null-terminated SSID containing 1-32 bytes. */
    char ssid[CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE];

    /** Empty for an open network, otherwise a null-terminated 8-63 byte key. */
    char password[CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE];
} config_manager_wifi_config_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Initialize the component mutex and runtime state.
 *
 * The application must initialize the default NVS flash partition before
 * calling storage APIs. Calling this function again after successful
 * initialization is safe and returns ESP_OK.
 *
 * @return ESP_OK on success or when already initialized,
 *         ESP_ERR_INVALID_STATE if another initialization is in progress, or
 *         ESP_ERR_NO_MEM if the mutex cannot be created.
 */
esp_err_t config_manager_init(void);

/**
 * @brief Validate and persist Wi-Fi credentials.
 *
 * The credentials and current configuration version are stored as separate
 * NVS keys and committed as one mutex-protected logical operation. Password
 * contents are never logged.
 *
 * @param[in] config Credentials to copy and save.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for malformed credentials,
 *         ESP_ERR_INVALID_STATE before initialization, ESP_ERR_TIMEOUT when
 *         the mutex cannot be acquired, or an NVS error.
 */
esp_err_t config_manager_save_wifi(
    const config_manager_wifi_config_t *config);

/**
 * @brief Load and validate persisted Wi-Fi credentials.
 *
 * The output is cleared before loading and remains cleared on every failure.
 * Only the current configuration version is accepted.
 *
 * @param[out] config Destination for the copied credentials.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND for missing data,
 *         ESP_ERR_NOT_SUPPORTED for an incompatible version,
 *         ESP_ERR_INVALID_ARG when config is NULL, ESP_ERR_INVALID_STATE
 *         before initialization, ESP_ERR_TIMEOUT, or another NVS error.
 */
esp_err_t config_manager_load_wifi(
    config_manager_wifi_config_t *config);

/**
 * @brief Erase Wi-Fi SSID and password keys.
 *
 * This operation is idempotent. It does not erase custom data or other future
 * device identity keys owned by the component.
 *
 * @return ESP_OK when the keys are absent or erased, ESP_ERR_INVALID_STATE
 *         before initialization, ESP_ERR_TIMEOUT, or an NVS error.
 */
esp_err_t config_manager_clear_wifi(void);

/**
 * @brief Check whether a complete, valid Wi-Fi configuration can be loaded.
 *
 * @param[out] has_config Set true only when config_manager_load_wifi()
 *             succeeds; false when data is missing.
 * @return ESP_OK for present or missing configuration, or the validation,
 *         version, synchronization, or NVS error for invalid stored data.
 */
esp_err_t config_manager_has_wifi_config(
    bool *has_config);

/**
 * @brief Save one typed value in the `custom_cfg` namespace.
 *
 * Integer sizes must exactly match the selected type. Strings must contain a
 * null terminator within value_size. Blobs must contain 1-512 bytes.
 *
 * @param[in] key Null-terminated NVS key containing 1-15 bytes.
 * @param[in] value Value bytes to copy into NVS.
 * @param[in] value_size Size supplied by the caller.
 * @param[in] type NVS representation to use.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for invalid pointers/key/type,
 *         ESP_ERR_INVALID_SIZE for a mismatched size, ESP_ERR_INVALID_STATE
 *         before initialization, ESP_ERR_TIMEOUT, or an NVS error.
 */
esp_err_t config_manager_save_custom_data(
    const char *key,
    const void *value,
    size_t value_size,
    config_manager_data_type_t type);

/**
 * @brief Load one typed value from the `custom_cfg` namespace.
 *
 * Fixed-size integers require a non-NULL output and exact input size. For a
 * string or blob, out_value may be NULL to query the required size. NVS updates
 * inout_size with the actual or required size where supported.
 *
 * @param[in] key Null-terminated NVS key containing 1-15 bytes.
 * @param[out] out_value Destination, or NULL for string/blob size queries.
 * @param[in,out] inout_size Available size on input and actual/required size
 *                on output.
 * @param[in] type Expected NVS representation.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND when absent,
 *         ESP_ERR_NVS_TYPE_MISMATCH for a stored-type mismatch,
 *         ESP_ERR_INVALID_ARG/ESP_ERR_INVALID_SIZE for an invalid request,
 *         ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, or another NVS error.
 */
esp_err_t config_manager_load_custom_data(
    const char *key,
    void *out_value,
    size_t *inout_size,
    config_manager_data_type_t type);

/**
 * @brief Erase one key from the `custom_cfg` namespace.
 *
 * The operation is idempotent and commits only when a key was erased.
 *
 * @param[in] key Null-terminated NVS key containing 1-15 bytes.
 * @return ESP_OK when absent or erased, ESP_ERR_INVALID_ARG for an invalid key,
 *         ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, or an NVS error.
 */
esp_err_t config_manager_clear_custom_data(
    const char *key);

esp_err_t config_manager_get_wifi_config_state(
    config_manager_wifi_config_state_t *state);

#ifdef __cplusplus
}
#endif
