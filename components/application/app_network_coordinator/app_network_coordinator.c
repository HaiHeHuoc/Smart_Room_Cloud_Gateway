#include "app_network_coordinator.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "config_manager.h"
#include "provisioning_manager.h"
#include "wifi_manager.h"

/* Macros ------------------------------------------------------------------- */
#define APP_NETWORK_COORDINATOR_TASK_NAME \
    "app_net_coord"

#define APP_NETWORK_COORDINATOR_TASK_STACK_SIZE_BYTES \
    (6U * 1024U)

#define APP_NETWORK_COORDINATOR_TASK_PRIORITY \
    4U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_NETWORK_COORDINATOR";

/* Static Variables --------------------------------------------------------- */
static app_network_coordinator_state_t s_state =
    APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED;

static app_network_coordinator_config_t s_config =
    {0};

static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

/* Function Prototypes ------------------------------------------------------ */
/**
 * @brief Resolve the persisted Wi-Fi state, migrating a supported legacy
 * schema when required.
 *
 * @param[out] state Final configuration state.
 *
 * @return ESP_OK on success, or an error from config_manager.
 */
static esp_err_t app_resolve_wifi_config_state(
    config_manager_wifi_config_state_t *state);

/**
 * @brief Load persisted credentials and request a Station connection.
 *
 * Temporary credential storage is cleared before this function returns.
 *
 * @return ESP_OK when the asynchronous connection request starts.
 */
static esp_err_t app_connect_stored_wifi(void);

/**
 * @brief Log one resolved Wi-Fi configuration state without sensitive data.
 *
 * @param[in] state State to report.
 */
static void app_log_wifi_config_state(
    config_manager_wifi_config_state_t state);

/**
 * @brief Securely overwrite a temporary buffer.
 *
 * @param[in,out] buffer Buffer to clear.
 * @param[in] size Buffer size in bytes.
 */
static void app_zeroize(
    void *buffer,
    size_t size);

/**
 * @brief Persist provisioned credentials and verify NVS state and read-back.
 *
 * @param[in] credentials Application-owned credential copy.
 *
 * @return ESP_OK only after save, state validation, and read-back match.
 */
static esp_err_t app_persist_and_verify_provisioned_wifi(
    const provisioning_manager_wifi_credentials_t *credentials);

/**
 * @brief Run one bounded provisioning, persistence, cleanup, and adoption flow.
 *
 * @return ESP_OK after wifi_manager adopts the active connection.
 */
static esp_err_t app_run_wifi_provisioning(void);

/**
 * @brief Apply boot policy for one resolved persistent configuration state.
 *
 * @param[in] state Resolved Wi-Fi configuration state.
 *
 * @return ESP_OK when the selected asynchronous network path starts.
 */
static esp_err_t app_apply_wifi_boot_policy(
    config_manager_wifi_config_state_t state);

/**
 * @brief Replace coordinator lifecycle state atomically.
 *
 * @param[in] state New coordinator state.
 */
static void app_network_coordinator_set_state(
    app_network_coordinator_state_t state);

/**
 * @brief Execute the synchronous boot policy in coordinator task context.
 *
 * @return ESP_OK when the selected network path starts or completes.
 */
static esp_err_t app_network_coordinator_run_boot_policy(void);

/**
 * @brief Run one-shot network boot orchestration without blocking app_main.
 *
 * @param[in] argument Unused.
 */
static void app_network_coordinator_task(
    void *argument);

/* Static Functions --------------------------------------------------------- */
static void app_network_coordinator_task(
    void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Network coordinator task started");

    const esp_err_t ret =
        app_network_coordinator_run_boot_policy();

    if (ret != ESP_OK)
    {
        app_network_coordinator_set_state(
            APP_NETWORK_COORDINATOR_STATE_FAILED);

        ESP_LOGE(
            TAG,
            "Network coordinator task failed: %s",
            esp_err_to_name(ret));
    }
    else
    {
        app_network_coordinator_state_t final_state =
            APP_NETWORK_COORDINATOR_STATE_FAILED;

        if (app_network_coordinator_get_state(
                &final_state) == ESP_OK)
        {
            ESP_LOGI(
                TAG,
                "Network coordinator boot task completed: state=%s",
                app_network_coordinator_state_to_string(
                    final_state));
        }
    }

    vTaskDelete(NULL);
}

static esp_err_t app_network_coordinator_run_boot_policy(void)
{
    app_network_coordinator_set_state(
        APP_NETWORK_COORDINATOR_STATE_RESOLVING_CONFIG);

    config_manager_wifi_config_state_t config_state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t ret =
        app_resolve_wifi_config_state(
            &config_state);

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to resolve Wi-Fi configuration: %s",
            esp_err_to_name(ret));

        return ret;
    }

    app_log_wifi_config_state(
        config_state);

    ret =
        app_apply_wifi_boot_policy(
            config_state);

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Network boot policy failed: %s",
            esp_err_to_name(ret));

        return ret;
    }

    ESP_LOGI(
        TAG,
        "Network boot policy started successfully");

    return ESP_OK;
}

static void app_network_coordinator_set_state(
    app_network_coordinator_state_t state)
{
    portENTER_CRITICAL(&s_state_lock);

    s_state = state;

    portEXIT_CRITICAL(&s_state_lock);
}

static void app_zeroize(
    void *buffer,
    size_t size)
{
    volatile uint8_t *bytes =
        (volatile uint8_t *)buffer;

    while (size > 0U)
    {
        *bytes++ = 0U;
        size--;
    }
}

static void app_log_wifi_config_state(
    config_manager_wifi_config_state_t state)
{
    switch (state)
    {
        case CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID:
            ESP_LOGI(TAG, "Stored Wi-Fi configuration is valid");
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED:
            ESP_LOGW(
                TAG,
                "Wi-Fi configuration is required; connection is not started");
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE:
            ESP_LOGE(
                TAG,
                "Stored Wi-Fi configuration is incomplete; data is preserved");
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION:
            ESP_LOGE(
                TAG,
                "Stored Wi-Fi schema is unsupported; data is preserved");
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA:
            ESP_LOGE(
                TAG,
                "Stored Wi-Fi configuration is invalid; data is preserved");
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED:
            ESP_LOGE(
                TAG,
                "Stored Wi-Fi configuration still requires migration");
            break;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN:
        default:
            ESP_LOGE(TAG, "Stored Wi-Fi configuration state is unknown");
            break;
    }
}

static esp_err_t app_persist_and_verify_provisioned_wifi(
    const provisioning_manager_wifi_credentials_t *credentials)
{
    if (credentials == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    config_manager_wifi_config_t config_to_save =
        {0};

    config_manager_wifi_config_t config_loaded =
        {0};

    config_manager_wifi_config_state_t config_state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t ret =
        ESP_FAIL;

    const size_t ssid_length =
        strnlen(
            credentials->ssid,
            sizeof(credentials->ssid));

    const size_t password_length =
        strnlen(
            credentials->password,
            sizeof(credentials->password));

    /*
     * provisioning_manager already validates the credentials before queueing
     * them. These checks protect this application boundary from malformed or
     * unterminated input.
     */
    if ((ssid_length == 0U) ||
        (ssid_length >= sizeof(credentials->ssid)) ||
        (password_length >= sizeof(credentials->password)))
    {
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    memcpy(
        config_to_save.ssid,
        credentials->ssid,
        ssid_length + 1U);

    memcpy(
        config_to_save.password,
        credentials->password,
        password_length + 1U);

    ret =
        config_manager_save_wifi(
            &config_to_save);

    if (ret != ESP_OK)
    {
        goto cleanup;
    }

    /*
     * Saving successfully is not enough. Re-inspect the schema and require a
     * complete current configuration before continuing.
     */
    ret =
        config_manager_get_wifi_config_state(
            &config_state);

    if (ret != ESP_OK)
    {
        goto cleanup;
    }

    if (config_state !=
        CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID)
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    /*
     * Read the data back from NVS rather than trusting the input buffer.
     */
    ret =
        config_manager_load_wifi(
            &config_loaded);

    if (ret != ESP_OK)
    {
        goto cleanup;
    }

    if ((strcmp(
             config_loaded.ssid,
             credentials->ssid) != 0) ||
        (strcmp(
             config_loaded.password,
             credentials->password) != 0))
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ret = ESP_OK;

cleanup:
    /*
     * Remove every temporary copy regardless of which operation failed.
     */
    app_zeroize(
        &config_to_save,
        sizeof(config_to_save));

    app_zeroize(
        &config_loaded,
        sizeof(config_loaded));

    return ret;
}

static esp_err_t app_run_wifi_provisioning(void)
{
    esp_err_t ret =
        provisioning_manager_init();

    if (ret != ESP_OK)
    {
        return ret;
    }

    ret =
        provisioning_manager_start();

    if (ret != ESP_OK)
    {
        return ret;
    }

    provisioning_manager_wifi_credentials_t credentials =
        {0};

    ret =
        provisioning_manager_receive_wifi_credentials(
            &credentials,
            s_config.provisioning_timeout_ms);

    if (ret != ESP_OK)
    {
        app_zeroize(
            &credentials,
            sizeof(credentials));

        /*
         * Request cleanup only when the provisioning service remains active.
         */
        provisioning_manager_state_t state =
            PROVISIONING_MANAGER_STATE_UNINITIALIZED;

        if ((provisioning_manager_get_state(&state) == ESP_OK) &&
            (state == PROVISIONING_MANAGER_STATE_ACTIVE))
        {
            (void)provisioning_manager_stop();
        }

        return ret;
    }

    ret =
        app_persist_and_verify_provisioned_wifi(
            &credentials);

    app_zeroize(
        &credentials,
        sizeof(credentials));

    if (ret != ESP_OK)
    {
        return ret;
    }

    TickType_t wait_start_tick =
        xTaskGetTickCount();

    while (1)
    {
        provisioning_manager_state_t provisioning_state =
            PROVISIONING_MANAGER_STATE_UNINITIALIZED;

        ret =
            provisioning_manager_get_state(
                &provisioning_state);

        if (ret != ESP_OK)
        {
            return ret;
        }

        if (provisioning_state ==
            PROVISIONING_MANAGER_STATE_STOPPED)
        {
            wifi_manager_status_t wifi_status = {0};

            ret =
                wifi_manager_get_status(
                    &wifi_status);

            if (ret != ESP_OK)
            {
                return ret;
            }

            if ((wifi_status.state ==
                 WIFI_MANAGER_STATE_CONNECTED) &&
                wifi_status.has_ipv4_address)
            {
                ret =
                    wifi_manager_adopt_active_connection();

                if (ret != ESP_OK)
                {
                    ESP_LOGE(
                        TAG,
                        "Failed to hand provisioning connection to Wi-Fi manager: %s",
                        esp_err_to_name(ret));

                    return ret;
                }

                ESP_LOGI(
                    TAG,
                    "Wi-Fi provisioning completed successfully");

                return ESP_OK;
            }
        }

        if (provisioning_state ==
            PROVISIONING_MANAGER_STATE_FAILED)
        {
            return ESP_FAIL;
        }

        TickType_t elapsed_ticks =
            xTaskGetTickCount() -
            wait_start_tick;

        if (elapsed_ticks >=
            pdMS_TO_TICKS(
                s_config.provisioning_timeout_ms))
        {
            ESP_LOGE(
                TAG,
                "Timed out waiting for provisioning completion");

            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                s_config.provisioning_poll_period_ms));
    }
}

static esp_err_t app_apply_wifi_boot_policy(
    config_manager_wifi_config_state_t state)
{
    switch (state)
    {
        case CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID:
            ESP_LOGI(
                TAG,
                "Valid Wi-Fi configuration found; "
                "starting stored connection");

            {
                esp_err_t ret =
                    app_connect_stored_wifi();

                if (ret == ESP_OK)
                {
                    app_network_coordinator_set_state(
                        APP_NETWORK_COORDINATOR_STATE_CONNECTING);
                }

                return ret;
            }

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED:
        {
            app_network_coordinator_set_state(
                APP_NETWORK_COORDINATOR_STATE_PROVISIONING);

            ESP_LOGI(
                TAG,
                "Wi-Fi is not configured; "
                "starting BLE provisioning");

            esp_err_t ret =
                app_run_wifi_provisioning();

            if (ret == ESP_OK)
            {
                app_network_coordinator_set_state(
                    APP_NETWORK_COORDINATOR_STATE_ONLINE);
            }

            return ret;
        }

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED:
        {
            ESP_LOGI(
                TAG,
                "Legacy Wi-Fi configuration requires migration");

            esp_err_t ret =
                config_manager_migrate_device_config();

            if (ret != ESP_OK)
            {
                return ret;
            }

            config_manager_wifi_config_state_t migrated_state =
                CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

            ret =
                config_manager_get_wifi_config_state(
                    &migrated_state);

            if (ret != ESP_OK)
            {
                return ret;
            }

            if (migrated_state !=
                CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID)
            {
                return ESP_ERR_INVALID_RESPONSE;
            }

            ret =
                app_connect_stored_wifi();

            if (ret == ESP_OK)
            {
                app_network_coordinator_set_state(
                    APP_NETWORK_COORDINATOR_STATE_CONNECTING);
            }

            return ret;
        }

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE:
            ESP_LOGE(
                TAG,
                "Stored Wi-Fi configuration is incomplete");

            return ESP_ERR_INVALID_STATE;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA:
            ESP_LOGE(
                TAG,
                "Stored Wi-Fi configuration contains invalid data");

            return ESP_ERR_INVALID_RESPONSE;

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION:
            ESP_LOGE(
                TAG,
                "Stored Wi-Fi configuration version is unsupported");

            return ESP_ERR_NOT_SUPPORTED;


        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN:
        default:
            ESP_LOGE(
                TAG,
                "Wi-Fi configuration state is unknown");

            return ESP_FAIL;
    }
}


static esp_err_t app_connect_stored_wifi(void)
{
    config_manager_wifi_config_t stored_config = {0};

    esp_err_t err =
        config_manager_load_wifi(&stored_config);

    if (err == ESP_OK)
    {
        const wifi_manager_sta_config_t station_config =
        {
            .ssid = stored_config.ssid,
            .password = stored_config.password,
        };

        err = wifi_manager_connect(&station_config);
    }

    app_zeroize(
        &stored_config,
        sizeof(stored_config));

    return err;
}

static esp_err_t app_resolve_wifi_config_state(
    config_manager_wifi_config_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *state = CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    esp_err_t err =
        config_manager_get_wifi_config_state(state);

    if (err != ESP_OK)
    {
        return err;
    }

    if (*state !=
        CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Migrating legacy Wi-Fi configuration");

    err = config_manager_migrate_device_config();

    if (err != ESP_OK)
    {
        return err;
    }

    err = config_manager_get_wifi_config_state(state);

    if (err != ESP_OK)
    {
        return err;
    }

    if (*state != CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi configuration is not valid after migration: state=%d",
            (int)*state);

        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

/* Functions ---------------------------------------------------------------- */
esp_err_t app_network_coordinator_init(
    const app_network_coordinator_config_t *config)
{
    ESP_RETURN_ON_FALSE(
        config != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Config is set to NULL"
    );

    ESP_RETURN_ON_FALSE(
        (config->provisioning_timeout_ms > 0U) &&
            (config->provisioning_poll_period_ms > 0U),
        ESP_ERR_INVALID_ARG,
        TAG,
        "Provisioning timing values must be greater than zero"
    );

    portENTER_CRITICAL(&s_state_lock);

    if (s_state !=
        APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    s_config = *config;
    s_state = APP_NETWORK_COORDINATOR_STATE_READY;

    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(
        TAG,
        "Application network coordinator initialized");

    return ESP_OK;
}

esp_err_t app_network_coordinator_get_state(
    app_network_coordinator_state_t *state)
{
    ESP_RETURN_ON_FALSE(
        state != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "State is set to NULL"
    );

    portENTER_CRITICAL(&s_state_lock);

    *state = s_state;

    portEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
}

const char *app_network_coordinator_state_to_string(
    app_network_coordinator_state_t state)
{
    switch (state)
    {
        case APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED:
            return "UNINITIALIZED";

        case APP_NETWORK_COORDINATOR_STATE_READY:
            return "READY";

        case APP_NETWORK_COORDINATOR_STATE_STARTING:
            return "STARTING";

        case APP_NETWORK_COORDINATOR_STATE_RESOLVING_CONFIG:
            return "RESOLVING_CONFIG";

        case APP_NETWORK_COORDINATOR_STATE_PROVISIONING:
            return "PROVISIONING";

        case APP_NETWORK_COORDINATOR_STATE_CONNECTING:
            return "CONNECTING";

        case APP_NETWORK_COORDINATOR_STATE_ONLINE:
            return "ONLINE";

        case APP_NETWORK_COORDINATOR_STATE_OFFLINE:
            return "OFFLINE";

        case APP_NETWORK_COORDINATOR_STATE_FAILED:
            return "FAILED";

        default:
            return "UNKNOWN";
    }
}

esp_err_t app_network_coordinator_start(void)
{
    /*
     * Atomically claim READY -> STARTING.
     */
    portENTER_CRITICAL(&s_state_lock);

    if (s_state !=
        APP_NETWORK_COORDINATOR_STATE_READY)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    s_state =
        APP_NETWORK_COORDINATOR_STATE_STARTING;

    portEXIT_CRITICAL(&s_state_lock);

    const BaseType_t task_result =
        xTaskCreate(
            app_network_coordinator_task,
            APP_NETWORK_COORDINATOR_TASK_NAME,
            APP_NETWORK_COORDINATOR_TASK_STACK_SIZE_BYTES,
            NULL,
            APP_NETWORK_COORDINATOR_TASK_PRIORITY,
            NULL);

    if (task_result != pdPASS)
    {
        app_network_coordinator_set_state(
            APP_NETWORK_COORDINATOR_STATE_FAILED);

        ESP_LOGE(
            TAG,
            "Failed to create network coordinator task");

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Network coordinator task scheduled");

    return ESP_OK;
}
