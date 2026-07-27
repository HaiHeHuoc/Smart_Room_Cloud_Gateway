#include "app_network_coordinator.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "app_gui.h"
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

#define APP_NETWORK_COORDINATOR_SUCCESS_DWELL_MS \
    1500U

_Static_assert(
    UI_PROVISIONING_QR_PAYLOAD_BUFFER_SIZE >=
        PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE,
    "app_gui QR payload buffer is smaller than provisioning_manager output");

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_NETWORK_COORDINATOR";

/* Static Variables --------------------------------------------------------- */
static app_network_coordinator_state_t s_state =
    APP_NETWORK_COORDINATOR_STATE_UNINITIALIZED;

static app_network_coordinator_config_t s_config =
    {0};

static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static uint16_t s_provisioning_disconnect_reason = 0U;
static bool s_provisioning_success_dwell_active = false;

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
 * @brief Copy the active provisioning QR payload into the GUI queue.
 *
 * QR publication is best-effort and never changes network state. The
 * temporary copy is cleared before return and its contents are never logged.
 */
static void app_publish_provisioning_qr_payload(void);

/**
 * @brief Publish one non-sensitive provisioning UI snapshot without blocking.
 */
static void app_publish_provisioning_status(
    ui_provisioning_state_t state,
    esp_err_t last_error,
    uint16_t wifi_disconnect_reason);

/**
 * @brief Invalidate the GUI QR cache without calling LVGL.
 */
static void app_clear_provisioning_qr_payload(void);

/**
 * @brief Translate manager lifecycle facts into provisioning GUI progress.
 */
static void app_provisioning_progress_callback(
    const provisioning_manager_progress_status_t *status,
    void *user_data);

/**
 * @brief Wait for manager cleanup to reach STOPPED with a finite timeout.
 */
static esp_err_t app_wait_for_provisioning_stop(
    uint32_t timeout_ms);

/**
 * @brief Stop an active provisioning session and wait for cleanup.
 */
static esp_err_t app_cleanup_provisioning_session(void);

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
 * @brief Queue the initial application screen for a verified config result.
 *
 * GUI routing is best-effort and never blocks or changes network policy.
 *
 * @param[in] state Final configuration state after supported migration.
 */
static void app_network_coordinator_request_initial_screen(
    config_manager_wifi_config_state_t state);

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
        app_network_coordinator_request_initial_screen(
            CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN);

        ESP_LOGE(
            TAG,
            "Failed to resolve Wi-Fi configuration: %s",
            esp_err_to_name(ret));

        return ret;
    }

    app_log_wifi_config_state(
        config_state);

    app_network_coordinator_request_initial_screen(
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

static void app_network_coordinator_request_initial_screen(
    config_manager_wifi_config_state_t state)
{
    const app_gui_screen_id_t target_screen =
        state == CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED
            ? APP_GUI_SCREEN_PROVISIONING
            : APP_GUI_SCREEN_BOOT;

    const esp_err_t ret =
        app_gui_request_screen(target_screen);

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to queue initial application screen %d: %s",
            (int)target_screen,
            esp_err_to_name(ret));
    }
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

static void app_publish_provisioning_qr_payload(void)
{
    ui_provisioning_qr_payload_t qr_payload = {0};

    esp_err_t ret =
        provisioning_manager_get_qr_payload(
            qr_payload.payload,
            sizeof(qr_payload.payload));

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Active provisioning QR payload is unavailable: %s",
            esp_err_to_name(ret));

        app_zeroize(
            &qr_payload,
            sizeof(qr_payload));

        return;
    }

    ret =
        app_gui_post_provisioning_qr_payload(
            &qr_payload);

    app_zeroize(
        &qr_payload,
        sizeof(qr_payload));

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to queue active provisioning QR payload: %s",
            esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(
        TAG,
        "Active provisioning QR payload queued for GUI");
}

static void app_publish_provisioning_status(
    ui_provisioning_state_t state,
    esp_err_t last_error,
    uint16_t wifi_disconnect_reason)
{
    const ui_provisioning_status_t status =
    {
        .state = state,
        .last_error = last_error,
        .wifi_disconnect_reason = wifi_disconnect_reason,
    };

    const esp_err_t ret =
        app_gui_post_provisioning_status(
            &status);

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to queue provisioning UI state %d: %s",
            (int)state,
            esp_err_to_name(ret));
    }
}

static void app_clear_provisioning_qr_payload(void)
{
    const esp_err_t ret =
        app_gui_clear_provisioning_qr_payload();

    if (ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to invalidate provisioning QR payload: %s",
            esp_err_to_name(ret));
    }
}

static void app_provisioning_progress_callback(
    const provisioning_manager_progress_status_t *status,
    void *user_data)
{
    (void)user_data;

    if (status == NULL)
    {
        return;
    }

    uint16_t provisioning_disconnect_reason = 0U;

    portENTER_CRITICAL(&s_state_lock);

    if ((status->progress ==
         PROVISIONING_MANAGER_PROGRESS_STARTING) ||
        (status->progress ==
         PROVISIONING_MANAGER_PROGRESS_CREDENTIAL_RECEIVED))
    {
        s_provisioning_disconnect_reason = 0U;
    }

    provisioning_disconnect_reason =
        s_provisioning_disconnect_reason;

    portEXIT_CRITICAL(&s_state_lock);

    switch (status->progress)
    {
        case PROVISIONING_MANAGER_PROGRESS_STARTING:
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_STARTING,
                status->last_error,
                0U);
            break;

        case PROVISIONING_MANAGER_PROGRESS_WAITING_FOR_PHONE:
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_WAITING_FOR_PHONE,
                status->last_error,
                0U);
            break;

        case PROVISIONING_MANAGER_PROGRESS_CREDENTIAL_RECEIVED:
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_CREDENTIAL_RECEIVED,
                status->last_error,
                0U);
            break;

        case PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTING:
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_CONNECTING_WIFI,
                status->last_error,
                0U);
            break;

        case PROVISIONING_MANAGER_PROGRESS_WIFI_CREDENTIAL_FAILED:
            /*
             * A normal credential failure is not terminal for the BLE
             * session. Keep the QR cache valid so the phone can retry.
             */
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_FAILED,
                status->last_error,
                provisioning_disconnect_reason);
            break;

        case PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTED:
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_WAITING_FOR_IP,
                status->last_error,
                0U);
            break;

        case PROVISIONING_MANAGER_PROGRESS_STOPPING:
        case PROVISIONING_MANAGER_PROGRESS_STOPPED:
            app_clear_provisioning_qr_payload();
            break;

        case PROVISIONING_MANAGER_PROGRESS_FAILED:
            app_clear_provisioning_qr_payload();
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_FAILED,
                status->last_error,
                status->wifi_failure_reason);
            break;

        default:
            break;
    }
}

static esp_err_t app_wait_for_provisioning_stop(
    uint32_t timeout_ms)
{
    const TickType_t wait_start_tick =
        xTaskGetTickCount();

    while (true)
    {
        provisioning_manager_state_t state =
            PROVISIONING_MANAGER_STATE_UNINITIALIZED;

        esp_err_t ret =
            provisioning_manager_get_state(
                &state);

        if (ret != ESP_OK)
        {
            return ret;
        }

        if (state == PROVISIONING_MANAGER_STATE_STOPPED)
        {
            return ESP_OK;
        }

        if (state == PROVISIONING_MANAGER_STATE_FAILED)
        {
            return ESP_FAIL;
        }

        const TickType_t elapsed_ticks =
            xTaskGetTickCount() -
            wait_start_tick;

        if (elapsed_ticks >=
            pdMS_TO_TICKS(timeout_ms))
        {
            return ESP_ERR_TIMEOUT;
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                s_config.provisioning_poll_period_ms));
    }
}

static esp_err_t app_cleanup_provisioning_session(void)
{
    app_clear_provisioning_qr_payload();

    provisioning_manager_state_t state =
        PROVISIONING_MANAGER_STATE_UNINITIALIZED;

    esp_err_t ret =
        provisioning_manager_get_state(
            &state);

    if (ret != ESP_OK)
    {
        return ret;
    }

    if (state == PROVISIONING_MANAGER_STATE_ACTIVE)
    {
        ret = provisioning_manager_stop();

        if (ret != ESP_OK)
        {
            return ret;
        }

        state = PROVISIONING_MANAGER_STATE_STOPPING;
    }

    if (state == PROVISIONING_MANAGER_STATE_STOPPED)
    {
        return ESP_OK;
    }

    if (state == PROVISIONING_MANAGER_STATE_FAILED)
    {
        return ESP_FAIL;
    }

    if (state != PROVISIONING_MANAGER_STATE_STOPPING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return app_wait_for_provisioning_stop(
        s_config.provisioning_timeout_ms);
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
        provisioning_manager_register_progress_callback(
            app_provisioning_progress_callback,
            NULL);

    if (ret != ESP_OK)
    {
        app_publish_provisioning_status(
            UI_PROVISIONING_STATE_FAILED,
            ret,
            0U);

        return ret;
    }

    /*
     * A new session owns a new QR identity. Invalidate any stale GUI cache
     * before the manager publishes this session's payload.
     */
    app_clear_provisioning_qr_payload();
    app_publish_provisioning_status(
        UI_PROVISIONING_STATE_STARTING,
        ESP_OK,
        0U);

    ret =
        provisioning_manager_init();

    if (ret != ESP_OK)
    {
        app_clear_provisioning_qr_payload();
        app_publish_provisioning_status(
            UI_PROVISIONING_STATE_FAILED,
            ret,
            0U);

        return ret;
    }

    ret =
        provisioning_manager_start();

    if (ret != ESP_OK)
    {
        app_clear_provisioning_qr_payload();
        app_publish_provisioning_status(
            UI_PROVISIONING_STATE_FAILED,
            ret,
            0U);

        return ret;
    }

    app_publish_provisioning_qr_payload();
    app_publish_provisioning_status(
        UI_PROVISIONING_STATE_WAITING_FOR_PHONE,
        ESP_OK,
        0U);

    provisioning_manager_wifi_credentials_t credentials =
        {0};

    bool connection_grace_used = false;

    ret =
        provisioning_manager_receive_wifi_credentials(
            &credentials,
            s_config.provisioning_timeout_ms);

    if (ret == ESP_ERR_TIMEOUT)
    {
        bool handoff_pending = false;

        const esp_err_t progress_ret =
            provisioning_manager_is_wifi_handoff_pending(
                &handoff_pending);

        if (progress_ret != ESP_OK)
        {
            ret = progress_ret;
        }
        else if (handoff_pending)
        {
            /*
             * The session deadline may expire after credentials are accepted
             * but just before DHCP completes. Keep the coordinator alive for
             * one bounded connection grace so the verified handoff can still
             * be persisted, cleaned up, and adopted safely.
             */
            ESP_LOGW(
                TAG,
                "Provisioning deadline reached with Wi-Fi handoff pending; "
                "waiting up to %lu ms for connection completion",
                (unsigned long)
                    s_config.provisioning_connection_grace_ms);

            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_WAITING_FOR_IP,
                ESP_OK,
                0U);

            connection_grace_used = true;

            ret =
                provisioning_manager_receive_wifi_credentials(
                    &credentials,
                    s_config.provisioning_connection_grace_ms);
        }
    }

    if (ret != ESP_OK)
    {
        app_zeroize(
            &credentials,
            sizeof(credentials));

        if (ret == ESP_ERR_TIMEOUT)
        {
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_TIMEOUT,
                ret,
                0U);
        }
        else
        {
            app_publish_provisioning_status(
                UI_PROVISIONING_STATE_FAILED,
                ret,
                0U);
        }

        const esp_err_t cleanup_ret =
            app_cleanup_provisioning_session();

        if (cleanup_ret != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Provisioning cleanup after receive failure did not complete: %s",
                esp_err_to_name(cleanup_ret));
        }

        return ret;
    }

    if (connection_grace_used)
    {
        ESP_LOGI(
            TAG,
            "Provisioning Wi-Fi handoff completed during connection grace");
    }

    app_publish_provisioning_status(
        UI_PROVISIONING_STATE_SAVING_CONFIG,
        ESP_OK,
        0U);

    ret =
        app_persist_and_verify_provisioned_wifi(
            &credentials);

    app_zeroize(
        &credentials,
        sizeof(credentials));

    if (ret != ESP_OK)
    {
        app_publish_provisioning_status(
            UI_PROVISIONING_STATE_FAILED,
            ret,
            0U);

        const esp_err_t cleanup_ret =
            app_cleanup_provisioning_session();

        if (cleanup_ret != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Provisioning cleanup after persistence failure did not complete: %s",
                esp_err_to_name(cleanup_ret));
        }

        return ret;
    }

    app_publish_provisioning_status(
        UI_PROVISIONING_STATE_CLEANING_UP,
        ESP_OK,
        0U);

    ret =
        app_wait_for_provisioning_stop(
            s_config.provisioning_timeout_ms);

    if (ret != ESP_OK)
    {
        app_clear_provisioning_qr_payload();
        app_publish_provisioning_status(
            (ret == ESP_ERR_TIMEOUT)
                ? UI_PROVISIONING_STATE_TIMEOUT
                : UI_PROVISIONING_STATE_FAILED,
            ret,
            0U);

        const esp_err_t cleanup_ret =
            app_cleanup_provisioning_session();

        if (cleanup_ret != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Provisioning cleanup did not reach STOPPED: %s",
                esp_err_to_name(cleanup_ret));
        }

        return ret;
    }

    wifi_manager_status_t wifi_status = {0};

    ret =
        wifi_manager_get_status(
            &wifi_status);

    if (ret != ESP_OK)
    {
        app_publish_provisioning_status(
            UI_PROVISIONING_STATE_FAILED,
            ret,
            0U);

        return ret;
    }

    if ((wifi_status.state !=
         WIFI_MANAGER_STATE_CONNECTED) ||
        !wifi_status.has_ipv4_address)
    {
        ret = ESP_ERR_INVALID_STATE;

        app_publish_provisioning_status(
            UI_PROVISIONING_STATE_FAILED,
            ret,
            wifi_status.disconnect_reason);

        return ret;
    }

    ret =
        wifi_manager_adopt_active_connection();

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to hand provisioning connection to Wi-Fi manager: %s",
            esp_err_to_name(ret));

        app_publish_provisioning_status(
            UI_PROVISIONING_STATE_FAILED,
            ret,
            wifi_status.disconnect_reason);

        return ret;
    }

    /*
     * Normal Wi-Fi ownership begins at adoption, not after the UI dwell.
     * Runtime disconnect/reconnect callbacks must be able to update state
     * during the 1500 ms success presentation.
     */
    portENTER_CRITICAL(&s_state_lock);

    s_state =
        APP_NETWORK_COORDINATOR_STATE_ONLINE;
    s_provisioning_success_dwell_active = true;

    portEXIT_CRITICAL(&s_state_lock);

    app_clear_provisioning_qr_payload();
    app_publish_provisioning_status(
        UI_PROVISIONING_STATE_SUCCESS,
        ESP_OK,
        0U);

    ESP_LOGI(
        TAG,
        "Wi-Fi provisioning completed successfully");

    vTaskDelay(
        pdMS_TO_TICKS(
            APP_NETWORK_COORDINATOR_SUCCESS_DWELL_MS));

    const esp_err_t screen_ret =
        app_gui_request_screen(
            APP_GUI_SCREEN_WIFI_STATUS);

    if (screen_ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Failed to queue post-provisioning Wi-Fi status screen: %s",
            esp_err_to_name(screen_ret));
    }

    portENTER_CRITICAL(&s_state_lock);

    s_provisioning_success_dwell_active = false;

    portEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
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
            (config->provisioning_connection_grace_ms > 0U) &&
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
    s_provisioning_disconnect_reason = 0U;
    s_provisioning_success_dwell_active = false;
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

esp_err_t app_network_coordinator_notify_wifi_event(
    app_network_coordinator_wifi_event_t event,
    uint16_t disconnect_reason)
{
    app_network_coordinator_state_t next_state;

    switch (event)
    {
        case APP_NETWORK_COORDINATOR_WIFI_EVENT_CONNECTING:
        case APP_NETWORK_COORDINATOR_WIFI_EVENT_WAITING_FOR_IP:
            next_state =
                APP_NETWORK_COORDINATOR_STATE_CONNECTING;
            break;

        case APP_NETWORK_COORDINATOR_WIFI_EVENT_ONLINE:
            next_state =
                APP_NETWORK_COORDINATOR_STATE_ONLINE;
            break;

        case APP_NETWORK_COORDINATOR_WIFI_EVENT_OFFLINE:
            next_state =
                APP_NETWORK_COORDINATOR_STATE_OFFLINE;
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }

    app_network_coordinator_state_t previous_state;
    bool state_changed = false;
    bool provisioning_active = false;
    bool success_dwell_active = false;

    portENTER_CRITICAL(&s_state_lock);

    previous_state = s_state;
    provisioning_active =
        (s_state ==
         APP_NETWORK_COORDINATOR_STATE_PROVISIONING);
    success_dwell_active =
        s_provisioning_success_dwell_active;

    if (provisioning_active &&
        (event ==
         APP_NETWORK_COORDINATOR_WIFI_EVENT_OFFLINE))
    {
        s_provisioning_disconnect_reason =
            disconnect_reason;
    }

    /*
     * Runtime Wi-Fi events are valid only after normal Station ownership has
     * begun. During provisioning, transient GOT_IP/disconnect events belong to
     * the provisioning transaction and must not advertise application ONLINE
     * before persistence, BLE cleanup, and connection adoption complete.
     */
    const bool runtime_tracking_active =
        (s_state ==
         APP_NETWORK_COORDINATOR_STATE_CONNECTING) ||
        (s_state ==
         APP_NETWORK_COORDINATOR_STATE_ONLINE) ||
        (s_state ==
         APP_NETWORK_COORDINATOR_STATE_OFFLINE);

    if (runtime_tracking_active &&
        (s_state != next_state))
    {
        s_state = next_state;
        state_changed = true;
    }

    portEXIT_CRITICAL(&s_state_lock);

    if (provisioning_active)
    {
        switch (event)
        {
            case APP_NETWORK_COORDINATOR_WIFI_EVENT_CONNECTING:
                app_publish_provisioning_status(
                    UI_PROVISIONING_STATE_CONNECTING_WIFI,
                    ESP_OK,
                    0U);
                break;

            case APP_NETWORK_COORDINATOR_WIFI_EVENT_WAITING_FOR_IP:
                app_publish_provisioning_status(
                    UI_PROVISIONING_STATE_WAITING_FOR_IP,
                    ESP_OK,
                    0U);
                break;

            case APP_NETWORK_COORDINATOR_WIFI_EVENT_OFFLINE:
                app_publish_provisioning_status(
                    UI_PROVISIONING_STATE_FAILED,
                    ESP_FAIL,
                    disconnect_reason);
                break;

            case APP_NETWORK_COORDINATOR_WIFI_EVENT_ONLINE:
            default:
                break;
        }

        return ESP_OK;
    }

    if (state_changed)
    {
        ESP_LOGI(
            TAG,
            "Runtime network state: %s -> %s",
            app_network_coordinator_state_to_string(
                previous_state),
            app_network_coordinator_state_to_string(
                next_state));

        if (next_state ==
            APP_NETWORK_COORDINATOR_STATE_ONLINE &&
            !success_dwell_active)
        {
            const esp_err_t screen_ret =
                app_gui_request_screen(
                    APP_GUI_SCREEN_WIFI_STATUS);

            if (screen_ret != ESP_OK)
            {
                ESP_LOGW(
                    TAG,
                    "Failed to queue Wi-Fi status screen: %s",
                    esp_err_to_name(screen_ret));
            }
        }
    }

    return ESP_OK;
}
