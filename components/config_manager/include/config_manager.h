#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Macros ------------------------------------------------------------------- */
/** Maximum Wi-Fi SSID length, excluding the null terminator. */
#define CONFIG_MANAGER_WIFI_SSID_MAX_LEN       32U

/** Maximum Wi-Fi password length, excluding the null terminator. */
#define CONFIG_MANAGER_WIFI_PASSWORD_MAX_LEN   63U

/** Buffer size required for a maximum-length null-terminated SSID. */
#define CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE \
    (CONFIG_MANAGER_WIFI_SSID_MAX_LEN + 1U)

/** Buffer size required for a maximum-length null-terminated password. */
#define CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE \
    (CONFIG_MANAGER_WIFI_PASSWORD_MAX_LEN + 1U)

/** Maximum custom NVS key length, excluding the null terminator. */
#define CONFIG_MANAGER_CUSTOM_KEY_MAX_LEN      15U

/** Maximum blob accepted by config_manager_save_custom_data(). */
#define CONFIG_MANAGER_CUSTOM_BLOB_MAX_SIZE    512U

/** Maximum device ID length, excluding the null terminator. */
#define CONFIG_MANAGER_DEVICE_ID_MAX_LEN       36U

/** Maximum device name length, excluding the null terminator. */
#define CONFIG_MANAGER_DEVICE_NAME_MAX_LEN     32U

/** Buffer size for a maximum-length null-terminated device ID. */
#define CONFIG_MANAGER_DEVICE_ID_BUFFER_SIZE \
    (CONFIG_MANAGER_DEVICE_ID_MAX_LEN + 1U)

/** Buffer size for a maximum-length null-terminated device name. */
#define CONFIG_MANAGER_DEVICE_NAME_BUFFER_SIZE \
    (CONFIG_MANAGER_DEVICE_NAME_MAX_LEN + 1U)

#ifdef __cplusplus
extern "C" {
#endif

/* Type Definitions --------------------------------------------------------- */
/** @brief NVS value types supported by the generic custom-data APIs. */
typedef enum
{
    CONFIG_MANAGER_DATA_TYPE_U8 = 0, /**< Unsigned 8-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_I8,     /**< Signed 8-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_U16,    /**< Unsigned 16-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_I16,    /**< Signed 16-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_U32,    /**< Unsigned 32-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_I32,    /**< Signed 32-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_U64,    /**< Unsigned 64-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_I64,    /**< Signed 64-bit integer. */
    CONFIG_MANAGER_DATA_TYPE_STRING, /**< Null-terminated string. */
    CONFIG_MANAGER_DATA_TYPE_BLOB,   /**< Opaque byte sequence. */
} config_manager_data_type_t;

/** @brief Integrity states reported for the stored Wi-Fi configuration. */
typedef enum
{
    /** State is unavailable because inspection has not completed or failed. */
    CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN = 0,

    /** Neither Wi-Fi credential key is stored; `cfg_ver` may remain. */
    CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED,

    /** Version, SSID, and password are present, supported, and valid. */
    CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID,

    /** At least one credential exists, but another required key is missing. */
    CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE,

    /** Complete valid credentials use an unsupported non-legacy version. */
    CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION,

    /** A required key has the wrong type, length, or semantic value. */
    CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA,

    /**
     * Valid legacy credentials are present but require an explicit schema
     * migration before they can be loaded.
     */
    CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED,
} config_manager_wifi_config_state_t;

/** @brief Wi-Fi credentials copied to or from the `device_cfg` namespace. */
typedef struct
{
    /** Null-terminated SSID containing 1-32 bytes. */
    char ssid[CONFIG_MANAGER_WIFI_SSID_BUFFER_SIZE];

    /** Empty for an open network, otherwise a null-terminated 8-63 byte key. */
    char password[CONFIG_MANAGER_WIFI_PASSWORD_BUFFER_SIZE];
} config_manager_wifi_config_t;

/**
 * @brief Application-owned device identity stored in `device_cfg`.
 *
 * The component validates and persists these values but does not generate
 * them or assign identity policy.
 */
typedef struct
{
    /** Non-empty null-terminated device identifier containing 1-36 bytes. */
    char device_id[CONFIG_MANAGER_DEVICE_ID_BUFFER_SIZE];

    /** Non-empty null-terminated display name containing 1-32 bytes. */
    char device_name[CONFIG_MANAGER_DEVICE_NAME_BUFFER_SIZE];
} config_manager_device_identity_t;

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
 * The credentials and configuration version are stored as separate NVS keys
 * in one mutex-protected operation. A durable write-in-progress version marker
 * prevents boot from accepting a partially updated credential pair after a
 * reset or flash error. Saving an identical current configuration is a no-op
 * to avoid unnecessary flash writes. Password contents are never logged.
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
 * Only the current configuration version is accepted. Valid legacy data is
 * reported as ESP_ERR_INVALID_STATE and must be migrated explicitly.
 *
 * @param[out] config Destination for the copied credentials.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND for missing data,
 *         ESP_ERR_INVALID_STATE for incomplete data, migration-required data,
 *         or use before initialization,
 *         ESP_ERR_NOT_SUPPORTED for an incompatible version,
 *         ESP_ERR_INVALID_RESPONSE for invalid stored data,
 *         ESP_ERR_INVALID_ARG when config is NULL, ESP_ERR_TIMEOUT, or another
 *         NVS error.
 */
esp_err_t config_manager_load_wifi(
    config_manager_wifi_config_t *config);

/**
 * @brief Erase Wi-Fi SSID and password keys.
 *
 * This operation is idempotent. It preserves `cfg_ver`, custom data, and
 * device identity.
 *
 * @return ESP_OK when the keys are absent or erased, ESP_ERR_INVALID_STATE
 *         before initialization, ESP_ERR_TIMEOUT, or an NVS error.
 */
esp_err_t config_manager_clear_wifi(void);

/**
 * @brief Inspect stored Wi-Fi keys without returning credential contents.
 *
 * Semantic integrity problems are returned through @p state while the
 * function itself returns ESP_OK. Storage, synchronization, and lifecycle
 * failures return an error and leave @p state as
 * CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN.
 *
 * @param[out] state Destination for the classified Wi-Fi configuration state.
 * @return ESP_OK when inspection completes, ESP_ERR_INVALID_ARG when state is
 *         NULL, ESP_ERR_INVALID_STATE before initialization, ESP_ERR_TIMEOUT,
 *         or an NVS access error.
 */
esp_err_t config_manager_get_wifi_config_state(
    config_manager_wifi_config_state_t *state);

/**
 * @brief Explicitly migrate a supported legacy Wi-Fi schema to version 1.
 *
 * Read APIs never migrate implicitly. Valid legacy credentials are preserved
 * unchanged while `cfg_ver` is written and committed once. Calling this API
 * for an already-current configuration is an idempotent ESP_OK no-op.
 *
 * @return ESP_OK after migration or for an already-current configuration,
 *         ESP_ERR_NVS_NOT_FOUND when credentials are absent,
 *         ESP_ERR_INVALID_STATE for incomplete data or use before init,
 *         ESP_ERR_NOT_SUPPORTED for a newer unsupported schema,
 *         ESP_ERR_INVALID_RESPONSE for invalid stored data,
 *         ESP_ERR_TIMEOUT, or another NVS error.
 */
esp_err_t config_manager_migrate_device_config(void);

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
 * @brief Validate and persist application-owned device identity.
 *
 * Both identity keys are staged and committed once. Device identity is
 * optional and does not alter the Wi-Fi schema version.
 *
 * @param[in] identity Identity values to copy and save.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for malformed identity,
 *         ESP_ERR_INVALID_STATE before initialization, ESP_ERR_TIMEOUT, or an
 *         NVS error.
 */
esp_err_t config_manager_save_device_identity(
    const config_manager_device_identity_t *identity);

/**
 * @brief Load a complete and valid device identity.
 *
 * The output is cleared before NVS access and remains cleared on every
 * failure.
 *
 * @param[out] identity Destination for copied identity values.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND when both keys are absent,
 *         ESP_ERR_INVALID_STATE when exactly one key is present or before
 *         initialization, ESP_ERR_INVALID_RESPONSE for wrong-type, oversized,
 *         or invalid stored values, ESP_ERR_INVALID_ARG when identity is NULL,
 *         ESP_ERR_TIMEOUT, or another NVS error.
 */
esp_err_t config_manager_load_device_identity(
    config_manager_device_identity_t *identity);

/**
 * @brief Idempotently erase both device identity keys.
 *
 * Wi-Fi credentials, schema version, and custom data are preserved.
 *
 * @return ESP_OK when already absent or erased, ESP_ERR_INVALID_STATE before
 *         initialization, ESP_ERR_TIMEOUT, or an NVS error.
 */
esp_err_t config_manager_clear_device_identity(void);

/**
 * @brief Check whether a complete valid device identity can be loaded.
 *
 * @param[out] has_identity Set true only for a complete valid identity.
 * @return ESP_OK for present or missing identity, or the integrity,
 *         synchronization, lifecycle, or NVS error for other failures.
 */
esp_err_t config_manager_has_device_identity(
    bool *has_identity);

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
 * string or blob, out_value may be NULL to query the required size. A supplied
 * output buffer is cleared before NVS access and remains cleared on failure.
 * NVS updates inout_size with the actual or required size where supported.
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

/**
 * @brief Erase all configuration owned by this component.
 *
 * The `device_cfg` and `custom_cfg` namespaces are cleared independently.
 * This operation is idempotent, does not erase the complete NVS partition,
 * does not reboot, and does not call Wi-Fi or application APIs. Because NVS
 * has no cross-namespace transaction, a flash failure between namespace
 * commits can leave a partial reset. Both namespaces are attempted even when
 * clearing the first namespace fails; the first error is returned.
 *
 * @return ESP_OK when both namespaces are absent or cleared,
 *         ESP_ERR_INVALID_STATE before initialization, ESP_ERR_TIMEOUT, or
 *         the first NVS error encountered.
 */
esp_err_t config_manager_factory_reset(void);

#ifdef __cplusplus
}
#endif
