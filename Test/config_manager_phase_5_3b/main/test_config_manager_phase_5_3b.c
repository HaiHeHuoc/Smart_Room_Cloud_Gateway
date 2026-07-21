/* Includes ----------------------------------------------------------------- */
#include "config_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

#define TEST_CURRENT_CONFIG_VERSION 1U
#define TEST_UNSUPPORTED_CONFIG_VERSION 99U
#define TEST_STALE_OUTPUT_PATTERN 0xA5

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "CONFIG_MANAGER_TEST";

static const char TEST_WIFI_SSID[] = "phase-5-3b-test";
static const char TEST_WIFI_PASSWORD[] = "test-pass-123";
static const char TEST_SHORT_PASSWORD[] = "short";
static const char TEST_OVERSIZED_SSID[] =
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

/* Function Prototypes ------------------------------------------------------ */
static esp_err_t test_erase_key_if_present(
    nvs_handle_t handle,
    const char *key,
    bool *changed);

static esp_err_t test_raw_clear_wifi(void);

static esp_err_t test_inject_raw_wifi(
    const test_raw_wifi_config_t *raw_config);

static esp_err_t test_recreate_empty_nvs(void);

static esp_err_t test_read_raw_version(
    uint32_t *version);

static void test_make_wifi_config(
    config_manager_wifi_config_t *config,
    const char *ssid,
    const char *password);

static bool test_buffer_is_zero(
    const void *buffer,
    size_t size);

static const char *test_wifi_state_name(
    config_manager_wifi_config_state_t state);

static test_wifi_observation_t test_observe_wifi(void);

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
static void test_valid_secured_config(void);
static void test_valid_open_config(void);
static void test_missing_namespace(void);
static void test_not_configured_after_clear(void);
static void test_incomplete_missing_password(void);
static void test_incomplete_password_only(void);
static void test_incomplete_missing_version(void);
static void test_wrong_ssid_type(void);
static void test_oversized_ssid(void);
static void test_unsupported_version(void);
static void test_semantic_empty_ssid(void);
static void test_semantic_short_password(void);

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

    ESP_LOGW(TAG, "Starting destructive Phase 5.3B NVS tests");

    UNITY_BEGIN();

    RUN_TEST(test_operation_error_before_init);
    RUN_TEST(test_component_init);
    RUN_TEST(test_valid_secured_config);
    RUN_TEST(test_valid_open_config);
    RUN_TEST(test_missing_namespace);
    RUN_TEST(test_not_configured_after_clear);
    RUN_TEST(test_incomplete_missing_password);
    RUN_TEST(test_incomplete_password_only);
    RUN_TEST(test_incomplete_missing_version);
    RUN_TEST(test_wrong_ssid_type);
    RUN_TEST(test_oversized_ssid);
    RUN_TEST(test_unsupported_version);
    RUN_TEST(test_semantic_empty_ssid);
    RUN_TEST(test_semantic_short_password);

    const int failures = UNITY_END();

    err = test_raw_clear_wifi();

    config_manager_wifi_config_state_t final_state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t state_err = config_manager_get_wifi_config_state(&final_state);

    ESP_LOGI(
        TAG,
        "Final cleanup: clear=%s, state_result=%s, state=%s",
        esp_err_to_name(err),
        esp_err_to_name(state_err),
        test_wifi_state_name(final_state));

    ESP_LOGI(TAG, "Phase 5.3B test run complete: failures=%d", failures);
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

static void test_incomplete_missing_version(void)
{
    const test_raw_wifi_config_t raw_config = {
        .write_ssid = true,
        .ssid = TEST_WIFI_SSID,
        .write_password = true,
        .password = TEST_WIFI_PASSWORD,
    };

    test_run_semantic_case(
        "INCOMPLETE_MISSING_VERSION",
        &raw_config,
        CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE,
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

/* Functions ---------------------------------------------------------------- */
void setUp(void)
{
    esp_err_t err = test_raw_clear_wifi();
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
}

void tearDown(void)
{
    esp_err_t err = test_raw_clear_wifi();
    TEST_ASSERT_EQUAL_INT(ESP_OK, err);
}
