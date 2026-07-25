/* Includes ----------------------------------------------------------------- */
#include "config_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "unity.h"

/* Macros ------------------------------------------------------------------- */
#define TEST_NVS_NAMESPACE "device_cfg"
#define TEST_NVS_KEY_VERSION "cfg_ver"
#define TEST_NVS_KEY_WIFI_SSID "wifi_ssid"
#define TEST_NVS_KEY_WIFI_PASS "wifi_pass"
#define TEST_NVS_KEY_DEVICE_ID "device_id"
#define TEST_NVS_KEY_DEVICE_NAME "device_name"

#define TEST_CUSTOM_NVS_NAMESPACE "custom_cfg"
#define TEST_CUSTOM_KEY "test_value"

#define TEST_UNRELATED_NVS_NAMESPACE "other_owner"
#define TEST_UNRELATED_KEY "keep"

#define TEST_CURRENT_CONFIG_VERSION 1U
#define TEST_UNSUPPORTED_CONFIG_VERSION 99U
#define TEST_WRITE_IN_PROGRESS_CONFIG_VERSION UINT32_MAX
#define TEST_STALE_OUTPUT_PATTERN 0xA5
#define TEST_LOOP_COUNT 4U
#define TEST_CONCURRENCY_WORKER_COUNT 4U
#define TEST_CONCURRENCY_ITERATIONS 12U
#define TEST_CONCURRENCY_TASK_STACK_DEPTH 4096U
#define TEST_CONCURRENCY_TASK_PRIORITY 5U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "CONFIG_MANAGER_TEST";

static const char TEST_WIFI_SSID[] = "config-manager-test";
static const char TEST_WIFI_SSID_ALT[] = "config-manager-alt";
static const char TEST_WIFI_PASSWORD[] = "test-pass-123";
static const char TEST_SHORT_PASSWORD[] = "short";
static const char TEST_OVERSIZED_SSID[] =
    "123456789012345678901234567890123";
static const char TEST_DEVICE_ID[] =
    "12345678-1234-1234-1234-123456789abc";
static const char TEST_DEVICE_NAME[] = "Smart Room Test Gateway";
static const char TEST_OVERSIZED_DEVICE_ID[] =
    "1234567890123456789012345678901234567";
static const char TEST_OVERSIZED_DEVICE_NAME[] =
    "123456789012345678901234567890123";

/* Type Definitions --------------------------------------------------------- */
typedef struct
{
    bool write_version;
    uint32_t version;

    bool write_ssid;
    bool write_ssid_as_u32;
    const char *ssid;

    bool write_password;
    const char *password;
} test_raw_wifi_config_t;

typedef struct
{
    esp_err_t state_result;
    config_manager_wifi_config_state_t state;
    esp_err_t load_result;
    bool load_output_empty;
} test_wifi_observation_t;

typedef struct
{
    bool writer;
    uint32_t worker_index;
    QueueHandle_t result_queue;
} test_concurrency_context_t;

typedef struct
{
    esp_err_t error;
    uint32_t timeout_count;
    uint32_t worker_index;
} test_concurrency_result_t;

/* Function Prototypes ------------------------------------------------------ */
static esp_err_t test_erase_key_if_present(
    nvs_handle_t handle,
    const char *key,
    bool *changed);

static esp_err_t test_raw_clear_namespace(
    const char *namespace_name);

static esp_err_t test_raw_clear_wifi(void);

static esp_err_t test_raw_clear_owned_data(void);

static esp_err_t test_inject_raw_wifi(
    const test_raw_wifi_config_t *raw_config);

static esp_err_t test_inject_raw_identity(
    bool write_device_id,
    bool device_id_as_u32,
    const char *device_id,
    bool write_device_name,
    bool device_name_as_u32,
    const char *device_name);

static esp_err_t test_recreate_empty_nvs(void);

static esp_err_t test_read_raw_version(
    uint32_t *version);

static void test_make_wifi_config(
    config_manager_wifi_config_t *config,
    const char *ssid,
    const char *password);

static void test_make_identity(
    config_manager_device_identity_t *identity,
    const char *device_id,
    const char *device_name);

static bool test_buffer_is_zero(
    const void *buffer,
    size_t size);

static const char *test_wifi_state_name(
    config_manager_wifi_config_state_t state);

static test_wifi_observation_t test_observe_wifi(void);

static esp_err_t test_write_unrelated_value(
    uint32_t value);

static esp_err_t test_read_unrelated_value(
    uint32_t *value);

static void test_concurrency_task(
    void *argument);

static void test_run_valid_case(
    const char *test_name,
    const char *password);

static void test_run_semantic_case(
    const char *test_name,
    const test_raw_wifi_config_t *raw_config,
    config_manager_wifi_config_state_t expected_state,
    esp_err_t expected_load_result);

static void test_operation_error_before_init(void);
static void test_component_init(void);
static void test_repeated_init(void);
static void test_valid_secured_config(void);
static void test_valid_open_config(void);
static void test_missing_namespace(void);
static void test_not_configured_after_clear(void);
static void test_incomplete_missing_password(void);
static void test_incomplete_password_only(void);
static void test_legacy_missing_version(void);
static void test_wrong_ssid_type(void);
static void test_oversized_ssid(void);
static void test_unsupported_version(void);
static void test_interrupted_wifi_update_is_rejected(void);
static void test_semantic_empty_ssid(void);
static void test_semantic_short_password(void);
static void test_explicit_version_zero_requires_migration(void);
static void test_migrate_legacy_preserves_credentials(void);
static void test_migrate_no_config(void);
static void test_migrate_incomplete_config(void);
static void test_migrate_invalid_config(void);
static void test_migrate_unsupported_version(void);
static void test_identity_save_load(void);
static void test_identity_clear_is_idempotent(void);
static void test_identity_incomplete_id_only(void);
static void test_identity_incomplete_name_only(void);
static void test_identity_wrong_type(void);
static void test_identity_oversized_value(void);
static void test_custom_wrong_type_clears_output(void);
static void test_wifi_clear_preserves_custom_data(void);
static void test_factory_reset_populated_namespaces(void);
static void test_factory_reset_is_idempotent(void);
static void test_factory_reset_missing_namespaces(void);
static void test_wifi_save_load_clear_loop(void);
static void test_migration_loop(void);
static void test_identity_loop(void);
static void test_factory_reset_loop(void);
static void test_concurrent_operations(void);

/* Application -------------------------------------------------------------- */
void app_main(void)
{
    esp_err_t err = nvs_flash_erase();

    if (err == ESP_OK)
    {
        err = nvs_flash_init();
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Unable to prepare isolated test NVS: %s",
            esp_err_to_name(err));
        return;
    }

    ESP_LOGW(TAG, "Starting destructive config_manager tests");

    UNITY_BEGIN();

    RUN_TEST(test_operation_error_before_init);
    RUN_TEST(test_component_init);
    RUN_TEST(test_repeated_init);
    RUN_TEST(test_valid_secured_config);
    RUN_TEST(test_valid_open_config);
    RUN_TEST(test_missing_namespace);
    RUN_TEST(test_not_configured_after_clear);
    RUN_TEST(test_incomplete_missing_password);
    RUN_TEST(test_incomplete_password_only);
    RUN_TEST(test_legacy_missing_version);
    RUN_TEST(test_wrong_ssid_type);
    RUN_TEST(test_oversized_ssid);
    RUN_TEST(test_unsupported_version);
    RUN_TEST(test_interrupted_wifi_update_is_rejected);
    RUN_TEST(test_semantic_empty_ssid);
    RUN_TEST(test_semantic_short_password);
    RUN_TEST(test_explicit_version_zero_requires_migration);
    RUN_TEST(test_migrate_legacy_preserves_credentials);
    RUN_TEST(test_migrate_no_config);
    RUN_TEST(test_migrate_incomplete_config);
    RUN_TEST(test_migrate_invalid_config);
    RUN_TEST(test_migrate_unsupported_version);
    RUN_TEST(test_identity_save_load);
    RUN_TEST(test_identity_clear_is_idempotent);
    RUN_TEST(test_identity_incomplete_id_only);
    RUN_TEST(test_identity_incomplete_name_only);
    RUN_TEST(test_identity_wrong_type);
    RUN_TEST(test_identity_oversized_value);
    RUN_TEST(test_custom_wrong_type_clears_output);
    RUN_TEST(test_wifi_clear_preserves_custom_data);
    RUN_TEST(test_factory_reset_populated_namespaces);
    RUN_TEST(test_factory_reset_is_idempotent);
    RUN_TEST(test_factory_reset_missing_namespaces);
    RUN_TEST(test_wifi_save_load_clear_loop);
    RUN_TEST(test_migration_loop);
    RUN_TEST(test_identity_loop);
    RUN_TEST(test_factory_reset_loop);
    RUN_TEST(test_concurrent_operations);

    const int failures = UNITY_END();

    err = test_raw_clear_owned_data();

    config_manager_wifi_config_state_t final_state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t state_err = config_manager_get_wifi_config_state(&final_state);

    ESP_LOGI(
        TAG,
        "Final cleanup: clear=%s, state_result=%s, state=%s",
        esp_err_to_name(err),
        esp_err_to_name(state_err),
        test_wifi_state_name(final_state));

    ESP_LOGI(TAG, "Config manager test run complete: failures=%d", failures);
}

/* Static Functions --------------------------------------------------------- */
static esp_err_t test_erase_key_if_present(
    nvs_handle_t handle,
    const char *key,
    bool *changed)
{
    esp_err_t err = nvs_erase_key(handle, key);

    if (err == ESP_OK)
    {
        *changed = true;
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }

    return err;
}

static esp_err_t test_raw_clear_namespace(
    const char *namespace_name)
{
    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
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

    return err;
}

static esp_err_t test_raw_clear_wifi(void)
{
    nvs_handle_t handle = 0;
    bool changed = false;

    esp_err_t err = nvs_open(
        TEST_NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    err = test_erase_key_if_present(
        handle,
        TEST_NVS_KEY_VERSION,
        &changed);

    if (err == ESP_OK)
    {
        err = test_erase_key_if_present(
            handle,
            TEST_NVS_KEY_WIFI_SSID,
            &changed);
    }

    if (err == ESP_OK)
    {
        err = test_erase_key_if_present(
            handle,
            TEST_NVS_KEY_WIFI_PASS,
            &changed);
    }

    if (err == ESP_OK && changed)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    return err;
}

static esp_err_t test_raw_clear_owned_data(void)
{
    esp_err_t err =
        test_raw_clear_namespace(TEST_NVS_NAMESPACE);

    if (err == ESP_OK)
    {
        err = test_raw_clear_namespace(
            TEST_CUSTOM_NVS_NAMESPACE);
    }

    return err;
}

static esp_err_t test_inject_raw_wifi(
    const test_raw_wifi_config_t *raw_config)
{
    if (raw_config == NULL ||
        (raw_config->write_ssid &&
         !raw_config->write_ssid_as_u32 &&
         raw_config->ssid == NULL) ||
        (raw_config->write_password && raw_config->password == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = test_raw_clear_wifi();

    if (err != ESP_OK)
    {
        return err;
    }

    nvs_handle_t handle = 0;

    err = nvs_open(
        TEST_NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    if (raw_config->write_version)
    {
        err = nvs_set_u32(
            handle,
            TEST_NVS_KEY_VERSION,
            raw_config->version);
    }

    if (err == ESP_OK && raw_config->write_ssid)
    {
        if (raw_config->write_ssid_as_u32)
        {
            err = nvs_set_u32(
                handle,
                TEST_NVS_KEY_WIFI_SSID,
                0x12345678U);
        }
        else
        {
            err = nvs_set_str(
                handle,
                TEST_NVS_KEY_WIFI_SSID,
                raw_config->ssid);
        }
    }

    if (err == ESP_OK && raw_config->write_password)
    {
        err = nvs_set_str(
            handle,
            TEST_NVS_KEY_WIFI_PASS,
            raw_config->password);
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    return err;
}

static esp_err_t test_inject_raw_identity(
    bool write_device_id,
    bool device_id_as_u32,
    const char *device_id,
    bool write_device_name,
    bool device_name_as_u32,
    const char *device_name)
{
    if ((write_device_id &&
         !device_id_as_u32 &&
         device_id == NULL) ||
        (write_device_name &&
         !device_name_as_u32 &&
         device_name == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    bool changed = false;

    esp_err_t err = nvs_open(
        TEST_NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    err = test_erase_key_if_present(
        handle,
        TEST_NVS_KEY_DEVICE_ID,
        &changed);

    if (err == ESP_OK)
    {
        err = test_erase_key_if_present(
            handle,
            TEST_NVS_KEY_DEVICE_NAME,
            &changed);
    }

    if (err == ESP_OK && write_device_id)
    {
        if (device_id_as_u32)
        {
            err = nvs_set_u32(
                handle,
                TEST_NVS_KEY_DEVICE_ID,
                0x12345678U);
        }
        else
        {
            err = nvs_set_str(
                handle,
                TEST_NVS_KEY_DEVICE_ID,
                device_id);
        }
    }

    if (err == ESP_OK && write_device_name)
    {
        if (device_name_as_u32)
        {
            err = nvs_set_u32(
                handle,
                TEST_NVS_KEY_DEVICE_NAME,
                0x87654321U);
        }
        else
        {
            err = nvs_set_str(
                handle,
                TEST_NVS_KEY_DEVICE_NAME,
                device_name);
        }
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    return err;
}

static esp_err_t test_recreate_empty_nvs(void)
{
    esp_err_t err = nvs_flash_deinit();

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_flash_erase();

    if (err != ESP_OK)
    {
        return err;
    }

    return nvs_flash_init();
}

static esp_err_t test_read_raw_version(
    uint32_t *version)
{
    if (version == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
        TEST_NVS_NAMESPACE,
        NVS_READONLY,
        &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_get_u32(
        handle,
        TEST_NVS_KEY_VERSION,
        version);

    nvs_close(handle);

    return err;
}

static void test_make_wifi_config(
    config_manager_wifi_config_t *config,
    const char *ssid,
    const char *password)
{
    memset(config, 0, sizeof(*config));

    (void)snprintf(
        config->ssid,
        sizeof(config->ssid),
        "%s",
        ssid);

    (void)snprintf(
        config->password,
        sizeof(config->password),
        "%s",
        password);
}

static void test_make_identity(
    config_manager_device_identity_t *identity,
    const char *device_id,
    const char *device_name)
{
    memset(identity, 0, sizeof(*identity));

    (void)snprintf(
        identity->device_id,
        sizeof(identity->device_id),
        "%s",
        device_id);

    (void)snprintf(
        identity->device_name,
        sizeof(identity->device_name),
        "%s",
        device_name);
}

static bool test_buffer_is_zero(
    const void *buffer,
    size_t size)
{
    const uint8_t *bytes = (const uint8_t *)buffer;

    for (size_t index = 0U; index < size; index++)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }

    return true;
}

static const char *test_wifi_state_name(
    config_manager_wifi_config_state_t state)
{
    switch (state)
    {
        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN:
            return "UNKNOWN";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED:
            return "NOT_CONFIGURED";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID:
            return "VALID";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE:
            return "INCOMPLETE";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION:
            return "UNSUPPORTED_VERSION";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA:
            return "INVALID_DATA";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED:
            return "MIGRATION_REQUIRED";

        default:
            return "OUT_OF_RANGE";
    }
}

static test_wifi_observation_t test_observe_wifi(void)
{
    test_wifi_observation_t observation = {
        .state_result = ESP_FAIL,
        .state = CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN,
        .load_result = ESP_FAIL,
        .load_output_empty = false,
    };

    config_manager_wifi_config_t loaded_config;
    memset(&loaded_config, TEST_STALE_OUTPUT_PATTERN, sizeof(loaded_config));

    observation.state_result =
        config_manager_get_wifi_config_state(&observation.state);

    observation.load_result =
        config_manager_load_wifi(&loaded_config);

    observation.load_output_empty = test_buffer_is_zero(
        &loaded_config,
        sizeof(loaded_config));

    memset(&loaded_config, 0, sizeof(loaded_config));

    return observation;
}

static esp_err_t test_write_unrelated_value(
    uint32_t value)
{
    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
        TEST_UNRELATED_NVS_NAMESPACE,
        NVS_READWRITE,
        &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_u32(
        handle,
        TEST_UNRELATED_KEY,
        value);

    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    return err;
}

static esp_err_t test_read_unrelated_value(
    uint32_t *value)
{
    if (value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;

    esp_err_t err = nvs_open(
        TEST_UNRELATED_NVS_NAMESPACE,
        NVS_READONLY,
        &handle);

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_get_u32(
        handle,
        TEST_UNRELATED_KEY,
        value);

    nvs_close(handle);

    return err;
}

static void test_concurrency_task(
    void *argument)
{
    test_concurrency_context_t *context =
        (test_concurrency_context_t *)argument;

    test_concurrency_result_t result = {
        .error = ESP_OK,
        .timeout_count = 0U,
        .worker_index = context->worker_index,
    };

    for (uint32_t iteration = 0U;
         iteration < TEST_CONCURRENCY_ITERATIONS;
         iteration++)
    {
        esp_err_t err = ESP_OK;

        if (context->writer)
        {
            config_manager_wifi_config_t config;

            const char *ssid =
                ((iteration + context->worker_index) % 2U) == 0U
                    ? TEST_WIFI_SSID
                    : TEST_WIFI_SSID_ALT;

            test_make_wifi_config(
                &config,
                ssid,
                TEST_WIFI_PASSWORD);

            err = config_manager_save_wifi(&config);
            memset(&config, 0, sizeof(config));
        }
        else
        {
            config_manager_wifi_config_t config;
            memset(
                &config,
                TEST_STALE_OUTPUT_PATTERN,
                sizeof(config));

            err = config_manager_load_wifi(&config);

            if (err == ESP_OK)
            {
                const bool ssid_valid =
                    strcmp(config.ssid, TEST_WIFI_SSID) == 0 ||
                    strcmp(config.ssid, TEST_WIFI_SSID_ALT) == 0;

                const bool password_valid =
                    strcmp(config.password, TEST_WIFI_PASSWORD) == 0;

                if (!ssid_valid || !password_valid)
                {
                    result.error = ESP_ERR_INVALID_RESPONSE;
                }
            }
            else if (err == ESP_ERR_TIMEOUT &&
                     !test_buffer_is_zero(&config, sizeof(config)))
            {
                result.error = ESP_ERR_INVALID_RESPONSE;
            }

            memset(&config, 0, sizeof(config));
        }

        if (result.error != ESP_OK)
        {
            break;
        }

        if (err == ESP_ERR_TIMEOUT)
        {
            result.timeout_count++;
        }
        else if (err != ESP_OK)
        {
            result.error = err;
            break;
        }

        taskYIELD();
    }

    (void)xQueueSend(
        context->result_queue,
        &result,
        portMAX_DELAY);

    /*
     * The parent releases the worker after receiving its result. This keeps
     * task handles valid for timeout cleanup and prevents queue use-after-free.
     */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    vTaskDelete(NULL);
}

static void test_run_valid_case(
    const char *test_name,
    const char *password)
{
    config_manager_wifi_config_t saved_config;
    config_manager_wifi_config_t loaded_config;

    test_make_wifi_config(
        &saved_config,
        TEST_WIFI_SSID,
        password);

    memset(&loaded_config, TEST_STALE_OUTPUT_PATTERN, sizeof(loaded_config));

    esp_err_t save_result = config_manager_save_wifi(&saved_config);

    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t state_result =
        config_manager_get_wifi_config_state(&state);

    esp_err_t load_result = config_manager_load_wifi(&loaded_config);

    const bool ssid_matches =
        strcmp(saved_config.ssid, loaded_config.ssid) == 0;

    const bool password_matches =
        strcmp(saved_config.password, loaded_config.password) == 0;

    esp_err_t cleanup_result = test_raw_clear_wifi();

    memset(&saved_config, 0, sizeof(saved_config));
    memset(&loaded_config, 0, sizeof(loaded_config));

    ESP_LOGI(
        TAG,
        "%s: expected_state=VALID, actual_state=%s, "
        "expected_load=ESP_OK, actual_load=%s, password_match=%s",
        test_name,
        test_wifi_state_name(state),
        esp_err_to_name(load_result),
        password_matches ? "YES" : "NO");

    TEST_ASSERT_EQUAL_INT(ESP_OK, save_result);
    TEST_ASSERT_EQUAL_INT(ESP_OK, state_result);
    TEST_ASSERT_EQUAL_INT(CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID, state);
    TEST_ASSERT_EQUAL_INT(ESP_OK, load_result);
    TEST_ASSERT_TRUE(ssid_matches);
    TEST_ASSERT_TRUE(password_matches);
    TEST_ASSERT_EQUAL_INT(ESP_OK, cleanup_result);

    ESP_LOGI(TAG, "[PASS] %s", test_name);
}

static void test_run_semantic_case(
    const char *test_name,
    const test_raw_wifi_config_t *raw_config,
    config_manager_wifi_config_state_t expected_state,
    esp_err_t expected_load_result)
{
    esp_err_t inject_result = test_inject_raw_wifi(raw_config);

    TEST_ASSERT_EQUAL_INT(ESP_OK, inject_result);

    test_wifi_observation_t observation = test_observe_wifi();
    esp_err_t cleanup_result = test_raw_clear_wifi();

    ESP_LOGI(
        TAG,
        "%s: expected_state=%s, actual_state=%s, "
        "expected_load=%s, actual_load=%s, output_empty=%s",
        test_name,
        test_wifi_state_name(expected_state),
        test_wifi_state_name(observation.state),
        esp_err_to_name(expected_load_result),
        esp_err_to_name(observation.load_result),
        observation.load_output_empty ? "YES" : "NO");

    TEST_ASSERT_EQUAL_INT(ESP_OK, observation.state_result);
    TEST_ASSERT_EQUAL_INT(expected_state, observation.state);
    TEST_ASSERT_EQUAL_INT(expected_load_result, observation.load_result);
    TEST_ASSERT_TRUE(observation.load_output_empty);
    TEST_ASSERT_EQUAL_INT(ESP_OK, cleanup_result);

    ESP_LOGI(TAG, "[PASS] %s", test_name);
}

static void test_operation_error_before_init(void)
{
    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID;

    config_manager_wifi_config_t output;
    memset(&output, TEST_STALE_OUTPUT_PATTERN, sizeof(output));

    esp_err_t state_result =
        config_manager_get_wifi_config_state(&state);

    esp_err_t load_result = config_manager_load_wifi(&output);
    const bool output_empty = test_buffer_is_zero(&output, sizeof(output));

    memset(&output, 0, sizeof(output));

    ESP_LOGI(
        TAG,
        "OPERATION_ERROR_BEFORE_INIT: expected_state=UNKNOWN, "
        "actual_state=%s, expected_result=ESP_ERR_INVALID_STATE, "
        "state_result=%s, load_result=%s, output_empty=%s",
        test_wifi_state_name(state),
        esp_err_to_name(state_result),
        esp_err_to_name(load_result),
        output_empty ? "YES" : "NO");

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, state_result);
    TEST_ASSERT_EQUAL_INT(CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN, state);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, load_result);
    TEST_ASSERT_TRUE(output_empty);

    ESP_LOGI(TAG, "[PASS] OPERATION_ERROR_BEFORE_INIT");
}

static void test_component_init(void)
{
    esp_err_t err = config_manager_init();

    ESP_LOGI(
        TAG,
        "COMPONENT_INIT: expected=ESP_OK, actual=%s",
        esp_err_to_name(err));

    TEST_ASSERT_EQUAL_INT(ESP_OK, err);

    ESP_LOGI(TAG, "[PASS] COMPONENT_INIT");
}

static void test_repeated_init(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, config_manager_init());
    TEST_ASSERT_EQUAL_INT(ESP_OK, config_manager_init());
}

static void test_valid_secured_config(void)
{
    test_run_valid_case("VALID_SECURED", TEST_WIFI_PASSWORD);
}

static void test_valid_open_config(void)
{
    test_run_valid_case("VALID_OPEN", "");
}

static void test_missing_namespace(void)
{
    esp_err_t recreate_result = test_recreate_empty_nvs();

    TEST_ASSERT_EQUAL_INT(ESP_OK, recreate_result);

    test_wifi_observation_t observation = test_observe_wifi();

    ESP_LOGI(
        TAG,
        "MISSING_NAMESPACE: expected_state=NOT_CONFIGURED, "
        "actual_state=%s, expected_load=ESP_ERR_NVS_NOT_FOUND, "
        "actual_load=%s, output_empty=%s",
        test_wifi_state_name(observation.state),
        esp_err_to_name(observation.load_result),
        observation.load_output_empty ? "YES" : "NO");

    TEST_ASSERT_EQUAL_INT(ESP_OK, observation.state_result);
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED,
        observation.state);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NVS_NOT_FOUND, observation.load_result);
    TEST_ASSERT_TRUE(observation.load_output_empty);

    ESP_LOGI(TAG, "[PASS] MISSING_NAMESPACE");
}

static void test_not_configured_after_clear(void)
{
    config_manager_wifi_config_t config;

    test_make_wifi_config(
        &config,
        TEST_WIFI_SSID,
        TEST_WIFI_PASSWORD);

    esp_err_t save_result = config_manager_save_wifi(&config);
    esp_err_t clear_result = config_manager_clear_wifi();

    uint32_t retained_version = 0U;
    esp_err_t version_result = test_read_raw_version(&retained_version);

    test_wifi_observation_t observation = test_observe_wifi();
    esp_err_t cleanup_result = test_raw_clear_wifi();

    memset(&config, 0, sizeof(config));

    ESP_LOGI(
        TAG,
        "NOT_CONFIGURED_AFTER_CLEAR: cfg_ver_retained=%s, "
        "expected_state=NOT_CONFIGURED, actual_state=%s, "
        "expected_load=ESP_ERR_NVS_NOT_FOUND, actual_load=%s, "
        "output_empty=%s",
        (version_result == ESP_OK &&
         retained_version == TEST_CURRENT_CONFIG_VERSION) ? "YES" : "NO",
        test_wifi_state_name(observation.state),
        esp_err_to_name(observation.load_result),
        observation.load_output_empty ? "YES" : "NO");

    TEST_ASSERT_EQUAL_INT(ESP_OK, save_result);
    TEST_ASSERT_EQUAL_INT(ESP_OK, clear_result);
    TEST_ASSERT_EQUAL_INT(ESP_OK, version_result);
    TEST_ASSERT_EQUAL_UINT32(TEST_CURRENT_CONFIG_VERSION, retained_version);
    TEST_ASSERT_EQUAL_INT(ESP_OK, observation.state_result);
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED,
        observation.state);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NVS_NOT_FOUND, observation.load_result);
    TEST_ASSERT_TRUE(observation.load_output_empty);
    TEST_ASSERT_EQUAL_INT(ESP_OK, cleanup_result);

    ESP_LOGI(TAG, "[PASS] NOT_CONFIGURED_AFTER_CLEAR");
}

static void test_incomplete_missing_password(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_CURRENT_CONFIG_VERSION,
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
    };

    test_run_semantic_case(
        "INCOMPLETE_MISSING_PASSWORD",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE,
        ESP_ERR_INVALID_STATE);
}

static void test_incomplete_password_only(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "INCOMPLETE_PASSWORD_ONLY",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE,
        ESP_ERR_INVALID_STATE);
}

static void test_legacy_missing_version(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "LEGACY_MISSING_VERSION",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED,
        ESP_ERR_INVALID_STATE);
}

static void test_wrong_ssid_type(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_CURRENT_CONFIG_VERSION,
        .write_ssid = true,
        .write_ssid_as_u32 = true,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "WRONG_SSID_TYPE",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA,
        ESP_ERR_INVALID_RESPONSE);
}

static void test_oversized_ssid(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_CURRENT_CONFIG_VERSION,
        .write_ssid = true,
        .ssid = TEST_OVERSIZED_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "OVERSIZED_SSID",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA,
        ESP_ERR_INVALID_RESPONSE);
}

static void test_unsupported_version(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_UNSUPPORTED_CONFIG_VERSION,
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    esp_err_t inject_result = test_inject_raw_wifi(&raw_config);

    TEST_ASSERT_EQUAL_INT(ESP_OK, inject_result);

    test_wifi_observation_t observation = test_observe_wifi();

    config_manager_wifi_config_state_t state_after_load =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t second_state_result =
        config_manager_get_wifi_config_state(&state_after_load);

    esp_err_t cleanup_result = test_raw_clear_wifi();

    ESP_LOGI(
        TAG,
        "UNSUPPORTED_VERSION: expected_state=UNSUPPORTED_VERSION, "
        "actual_state=%s, state_after_load=%s, "
        "expected_load=ESP_ERR_NOT_SUPPORTED, actual_load=%s, "
        "output_empty=%s",
        test_wifi_state_name(observation.state),
        test_wifi_state_name(state_after_load),
        esp_err_to_name(observation.load_result),
        observation.load_output_empty ? "YES" : "NO");

    TEST_ASSERT_EQUAL_INT(ESP_OK, observation.state_result);
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION,
        observation.state);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_SUPPORTED, observation.load_result);
    TEST_ASSERT_TRUE(observation.load_output_empty);
    TEST_ASSERT_EQUAL_INT(ESP_OK, second_state_result);
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION,
        state_after_load);
    TEST_ASSERT_EQUAL_INT(ESP_OK, cleanup_result);

    ESP_LOGI(TAG, "[PASS] UNSUPPORTED_VERSION");
}

static void test_interrupted_wifi_update_is_rejected(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_WRITE_IN_PROGRESS_CONFIG_VERSION,
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "INTERRUPTED_WIFI_UPDATE",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION,
        ESP_ERR_NOT_SUPPORTED);
}

static void test_semantic_empty_ssid(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_CURRENT_CONFIG_VERSION,
        .write_ssid = true,
        .ssid = "",
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "SEMANTIC_EMPTY_SSID",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA,
        ESP_ERR_INVALID_RESPONSE);
}

static void test_semantic_short_password(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_CURRENT_CONFIG_VERSION,
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_SHORT_PASSWORD,
    };

    test_run_semantic_case(
        "SEMANTIC_SHORT_PASSWORD",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA,
        ESP_ERR_INVALID_RESPONSE);
}

static void test_explicit_version_zero_requires_migration(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = 0U,
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "EXPLICIT_VERSION_ZERO",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED,
        ESP_ERR_INVALID_STATE);
}

static void test_migrate_legacy_preserves_credentials(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_wifi(&raw_config));

    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_get_wifi_config_state(&state));
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED,
        state);

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_migrate_device_config());

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_get_wifi_config_state(&state));
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID,
        state);

    config_manager_wifi_config_t loaded_config = {0};

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_load_wifi(&loaded_config));
    TEST_ASSERT_EQUAL_STRING(TEST_WIFI_SSID, loaded_config.ssid);
    TEST_ASSERT_EQUAL_STRING(
        TEST_WIFI_PASSWORD,
        loaded_config.password);

    uint32_t stored_version = 0U;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_read_raw_version(&stored_version));
    TEST_ASSERT_EQUAL_UINT32(
        TEST_CURRENT_CONFIG_VERSION,
        stored_version);

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_migrate_device_config());

    memset(&loaded_config, 0, sizeof(loaded_config));
}

static void test_migrate_no_config(void)
{
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_raw_clear_wifi());
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NVS_NOT_FOUND,
        config_manager_migrate_device_config());
}

static void test_migrate_incomplete_config(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
    };

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_wifi(&raw_config));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_STATE,
        config_manager_migrate_device_config());
}

static void test_migrate_invalid_config(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_ssid = true,
        .ssid = "",
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_wifi(&raw_config));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_RESPONSE,
        config_manager_migrate_device_config());
}

static void test_migrate_unsupported_version(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_version = true,
        .version = TEST_UNSUPPORTED_CONFIG_VERSION,
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_wifi(&raw_config));
    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NOT_SUPPORTED,
        config_manager_migrate_device_config());

    uint32_t stored_version = 0U;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_read_raw_version(&stored_version));
    TEST_ASSERT_EQUAL_UINT32(
        TEST_UNSUPPORTED_CONFIG_VERSION,
        stored_version);
}

static void test_identity_save_load(void)
{
    config_manager_device_identity_t saved_identity;
    config_manager_device_identity_t loaded_identity;

    test_make_identity(
        &saved_identity,
        TEST_DEVICE_ID,
        TEST_DEVICE_NAME);

    memset(
        &loaded_identity,
        TEST_STALE_OUTPUT_PATTERN,
        sizeof(loaded_identity));

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_device_identity(&saved_identity));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_load_device_identity(&loaded_identity));
    TEST_ASSERT_EQUAL_STRING(
        saved_identity.device_id,
        loaded_identity.device_id);
    TEST_ASSERT_EQUAL_STRING(
        saved_identity.device_name,
        loaded_identity.device_name);

    bool has_identity = false;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_has_device_identity(&has_identity));
    TEST_ASSERT_TRUE(has_identity);

    memset(&saved_identity, 0, sizeof(saved_identity));
    memset(&loaded_identity, 0, sizeof(loaded_identity));
}

static void test_identity_clear_is_idempotent(void)
{
    config_manager_device_identity_t identity;

    test_make_identity(
        &identity,
        TEST_DEVICE_ID,
        TEST_DEVICE_NAME);

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_device_identity(&identity));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_clear_device_identity());
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_clear_device_identity());

    memset(
        &identity,
        TEST_STALE_OUTPUT_PATTERN,
        sizeof(identity));

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NVS_NOT_FOUND,
        config_manager_load_device_identity(&identity));
    TEST_ASSERT_TRUE(
        test_buffer_is_zero(&identity, sizeof(identity)));

    bool has_identity = true;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_has_device_identity(&has_identity));
    TEST_ASSERT_FALSE(has_identity);
}

static void test_identity_incomplete_id_only(void)
{
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_identity(
            true,
            false,
            TEST_DEVICE_ID,
            false,
            false,
            NULL));

    config_manager_device_identity_t identity;
    memset(&identity, TEST_STALE_OUTPUT_PATTERN, sizeof(identity));

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_STATE,
        config_manager_load_device_identity(&identity));
    TEST_ASSERT_TRUE(
        test_buffer_is_zero(&identity, sizeof(identity)));
}

static void test_identity_incomplete_name_only(void)
{
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_identity(
            false,
            false,
            NULL,
            true,
            false,
            TEST_DEVICE_NAME));

    config_manager_device_identity_t identity;
    memset(&identity, TEST_STALE_OUTPUT_PATTERN, sizeof(identity));

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_STATE,
        config_manager_load_device_identity(&identity));
    TEST_ASSERT_TRUE(
        test_buffer_is_zero(&identity, sizeof(identity)));
}

static void test_identity_wrong_type(void)
{
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_identity(
            true,
            true,
            NULL,
            true,
            false,
            TEST_DEVICE_NAME));

    config_manager_device_identity_t identity;
    memset(&identity, TEST_STALE_OUTPUT_PATTERN, sizeof(identity));

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_RESPONSE,
        config_manager_load_device_identity(&identity));
    TEST_ASSERT_TRUE(
        test_buffer_is_zero(&identity, sizeof(identity)));
}

static void test_identity_oversized_value(void)
{
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_inject_raw_identity(
            true,
            false,
            TEST_OVERSIZED_DEVICE_ID,
            true,
            false,
            TEST_OVERSIZED_DEVICE_NAME));

    config_manager_device_identity_t identity;
    memset(&identity, TEST_STALE_OUTPUT_PATTERN, sizeof(identity));

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_INVALID_RESPONSE,
        config_manager_load_device_identity(&identity));
    TEST_ASSERT_TRUE(
        test_buffer_is_zero(&identity, sizeof(identity)));
}

static void test_custom_wrong_type_clears_output(void)
{
    const uint32_t saved_value = 0xCAFEBABEU;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_custom_data(
            TEST_CUSTOM_KEY,
            &saved_value,
            sizeof(saved_value),
            CONFIG_MANAGER_DATA_TYPE_U32));

    char output[16];
    memset(output, TEST_STALE_OUTPUT_PATTERN, sizeof(output));

    size_t output_size = sizeof(output);

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NVS_TYPE_MISMATCH,
        config_manager_load_custom_data(
            TEST_CUSTOM_KEY,
            output,
            &output_size,
            CONFIG_MANAGER_DATA_TYPE_STRING));
    TEST_ASSERT_TRUE(
        test_buffer_is_zero(output, sizeof(output)));
}

static void test_wifi_clear_preserves_custom_data(void)
{
    config_manager_wifi_config_t config;

    test_make_wifi_config(
        &config,
        TEST_WIFI_SSID,
        TEST_WIFI_PASSWORD);

    const uint32_t saved_value = 0x5A5AA5A5U;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_wifi(&config));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_custom_data(
            TEST_CUSTOM_KEY,
            &saved_value,
            sizeof(saved_value),
            CONFIG_MANAGER_DATA_TYPE_U32));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_clear_wifi());

    uint32_t loaded_value = 0U;
    size_t loaded_size = sizeof(loaded_value);

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_load_custom_data(
            TEST_CUSTOM_KEY,
            &loaded_value,
            &loaded_size,
            CONFIG_MANAGER_DATA_TYPE_U32));
    TEST_ASSERT_EQUAL_UINT32(saved_value, loaded_value);

    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_get_wifi_config_state(&state));
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED,
        state);

    memset(&config, 0, sizeof(config));
}

static void test_factory_reset_populated_namespaces(void)
{
    config_manager_wifi_config_t wifi_config;
    config_manager_device_identity_t identity;

    test_make_wifi_config(
        &wifi_config,
        TEST_WIFI_SSID,
        TEST_WIFI_PASSWORD);
    test_make_identity(
        &identity,
        TEST_DEVICE_ID,
        TEST_DEVICE_NAME);

    const uint32_t custom_value = 0x11223344U;
    const uint32_t unrelated_value = 0xAABBCCDDU;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_wifi(&wifi_config));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_device_identity(&identity));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_custom_data(
            TEST_CUSTOM_KEY,
            &custom_value,
            sizeof(custom_value),
            CONFIG_MANAGER_DATA_TYPE_U32));
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_write_unrelated_value(unrelated_value));

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_factory_reset());

    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_get_wifi_config_state(&state));
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED,
        state);

    memset(&identity, TEST_STALE_OUTPUT_PATTERN, sizeof(identity));

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NVS_NOT_FOUND,
        config_manager_load_device_identity(&identity));
    TEST_ASSERT_TRUE(
        test_buffer_is_zero(&identity, sizeof(identity)));

    uint32_t loaded_custom = 0U;
    size_t loaded_custom_size = sizeof(loaded_custom);

    TEST_ASSERT_EQUAL_INT(
        ESP_ERR_NVS_NOT_FOUND,
        config_manager_load_custom_data(
            TEST_CUSTOM_KEY,
            &loaded_custom,
            &loaded_custom_size,
            CONFIG_MANAGER_DATA_TYPE_U32));

    uint32_t loaded_unrelated = 0U;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_read_unrelated_value(&loaded_unrelated));
    TEST_ASSERT_EQUAL_UINT32(
        unrelated_value,
        loaded_unrelated);

    memset(&wifi_config, 0, sizeof(wifi_config));
    memset(&identity, 0, sizeof(identity));
}

static void test_factory_reset_is_idempotent(void)
{
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_factory_reset());
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_factory_reset());
}

static void test_factory_reset_missing_namespaces(void)
{
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        test_recreate_empty_nvs());
    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_factory_reset());

    config_manager_wifi_config_state_t state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_get_wifi_config_state(&state));
    TEST_ASSERT_EQUAL_INT(
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED,
        state);
}

static void test_wifi_save_load_clear_loop(void)
{
    for (uint32_t iteration = 0U;
         iteration < TEST_LOOP_COUNT;
         iteration++)
    {
        config_manager_wifi_config_t saved_config;
        config_manager_wifi_config_t loaded_config;

        test_make_wifi_config(
            &saved_config,
            TEST_WIFI_SSID,
            TEST_WIFI_PASSWORD);

        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_save_wifi(&saved_config));
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_load_wifi(&loaded_config));
        TEST_ASSERT_EQUAL_STRING(
            saved_config.ssid,
            loaded_config.ssid);
        TEST_ASSERT_EQUAL_STRING(
            saved_config.password,
            loaded_config.password);
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_clear_wifi());

        memset(&saved_config, 0, sizeof(saved_config));
        memset(&loaded_config, 0, sizeof(loaded_config));
    }
}

static void test_migration_loop(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    for (uint32_t iteration = 0U;
         iteration < TEST_LOOP_COUNT;
         iteration++)
    {
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            test_inject_raw_wifi(&raw_config));
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_migrate_device_config());
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_migrate_device_config());

        config_manager_wifi_config_state_t state =
            CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_get_wifi_config_state(&state));
        TEST_ASSERT_EQUAL_INT(
            CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID,
            state);
    }
}

static void test_identity_loop(void)
{
    for (uint32_t iteration = 0U;
         iteration < TEST_LOOP_COUNT;
         iteration++)
    {
        config_manager_device_identity_t saved_identity;
        config_manager_device_identity_t loaded_identity;

        test_make_identity(
            &saved_identity,
            TEST_DEVICE_ID,
            TEST_DEVICE_NAME);

        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_save_device_identity(&saved_identity));
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_load_device_identity(&loaded_identity));
        TEST_ASSERT_EQUAL_STRING(
            saved_identity.device_id,
            loaded_identity.device_id);
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_clear_device_identity());

        memset(&saved_identity, 0, sizeof(saved_identity));
        memset(&loaded_identity, 0, sizeof(loaded_identity));
    }
}

static void test_factory_reset_loop(void)
{
    const uint32_t custom_value = 0x10203040U;

    for (uint32_t iteration = 0U;
         iteration < TEST_LOOP_COUNT;
         iteration++)
    {
        config_manager_device_identity_t identity;

        test_make_identity(
            &identity,
            TEST_DEVICE_ID,
            TEST_DEVICE_NAME);

        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_save_device_identity(&identity));
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_save_custom_data(
                TEST_CUSTOM_KEY,
                &custom_value,
                sizeof(custom_value),
                CONFIG_MANAGER_DATA_TYPE_U32));
        TEST_ASSERT_EQUAL_INT(
            ESP_OK,
            config_manager_factory_reset());

        memset(&identity, 0, sizeof(identity));
    }
}

static void test_concurrent_operations(void)
{
    config_manager_wifi_config_t initial_config;

    test_make_wifi_config(
        &initial_config,
        TEST_WIFI_SSID,
        TEST_WIFI_PASSWORD);

    TEST_ASSERT_EQUAL_INT(
        ESP_OK,
        config_manager_save_wifi(&initial_config));

    QueueHandle_t result_queue = xQueueCreate(
        TEST_CONCURRENCY_WORKER_COUNT,
        sizeof(test_concurrency_result_t));

    TEST_ASSERT_NOT_NULL(result_queue);

    test_concurrency_context_t contexts[
        TEST_CONCURRENCY_WORKER_COUNT] = {0};
    TaskHandle_t worker_handles[
        TEST_CONCURRENCY_WORKER_COUNT] = {0};
    bool worker_completed[
        TEST_CONCURRENCY_WORKER_COUNT] = {false};

    uint32_t created_count = 0U;

    for (uint32_t worker_index = 0U;
         worker_index < TEST_CONCURRENCY_WORKER_COUNT;
         worker_index++)
    {
        contexts[worker_index].writer =
            worker_index < 2U;
        contexts[worker_index].worker_index =
            worker_index;
        contexts[worker_index].result_queue =
            result_queue;

        BaseType_t create_result = xTaskCreate(
            test_concurrency_task,
            "cfg_test_worker",
            TEST_CONCURRENCY_TASK_STACK_DEPTH,
            &contexts[worker_index],
            TEST_CONCURRENCY_TASK_PRIORITY,
            &worker_handles[worker_index]);

        if (create_result == pdPASS)
        {
            created_count++;
        }
    }

    uint32_t received_count = 0U;
    uint32_t timeout_count = 0U;
    esp_err_t worker_error = ESP_OK;

    while (received_count < created_count)
    {
        test_concurrency_result_t result = {0};

        if (xQueueReceive(
                result_queue,
                &result,
                pdMS_TO_TICKS(15000U)) != pdTRUE)
        {
            worker_error = ESP_ERR_TIMEOUT;
            break;
        }

        if (result.worker_index >= TEST_CONCURRENCY_WORKER_COUNT ||
            worker_handles[result.worker_index] == NULL ||
            worker_completed[result.worker_index])
        {
            worker_error = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        worker_completed[result.worker_index] = true;
        xTaskNotifyGive(worker_handles[result.worker_index]);

        received_count++;
        timeout_count += result.timeout_count;

        if (result.error != ESP_OK)
        {
            worker_error = result.error;
        }
    }

    for (uint32_t worker_index = 0U;
         worker_index < TEST_CONCURRENCY_WORKER_COUNT;
         worker_index++)
    {
        if (worker_handles[worker_index] != NULL &&
            !worker_completed[worker_index])
        {
            vTaskDelete(worker_handles[worker_index]);
        }
    }

    vQueueDelete(result_queue);
    memset(&initial_config, 0, sizeof(initial_config));

    ESP_LOGI(
        TAG,
        "Concurrency: created=%u, completed=%u, lock_timeouts=%u",
        (unsigned int)created_count,
        (unsigned int)received_count,
        (unsigned int)timeout_count);

    TEST_ASSERT_EQUAL_UINT32(
        TEST_CONCURRENCY_WORKER_COUNT,
        created_count);
    TEST_ASSERT_EQUAL_UINT32(
        created_count,
        received_count);
    TEST_ASSERT_EQUAL_INT(ESP_OK, worker_error);
}

/* Functions ---------------------------------------------------------------- */
void setUp(void)
{
    esp_err_t err = test_raw_clear_owned_data();
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
}

void tearDown(void)
{
    esp_err_t err = test_raw_clear_owned_data();
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
}
