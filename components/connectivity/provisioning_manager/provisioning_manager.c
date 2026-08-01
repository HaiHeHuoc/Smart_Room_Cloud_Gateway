/* Includes ----------------------------------------------------------------- */
#include "provisioning_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi_types.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
/* Macros ------------------------------------------------------------------- */
#define PROVISIONING_MANAGER_SERVICE_NAME_BUFFER_SIZE      12U
#define PROVISIONING_MANAGER_CLEANUP_TASK_STACK_SIZE_BYTES (4U * 1024U)
#define PROVISIONING_MANAGER_CLEANUP_TASK_PRIORITY          4U

#define PROVISIONING_MANAGER_CREDENTIAL_QUEUE_LENGTH 1U

_Static_assert(
    PROVISIONING_MANAGER_CREDENTIAL_QUEUE_LENGTH == 1U,
    "Credential queue clearing requires a length-one queue");

/*
 * Match CONFIG_EXAMPLE_PROV_MGR_CONNECTION_CNT from Espressif's wifi_prov
 * example. These attempts belong to the provisioning framework while it
 * validates one received credential set; they do not replace wifi_manager's
 * normal reconnect policy after application adoption.
 */
#define PROVISIONING_MANAGER_WIFI_CONNECTION_ATTEMPTS 5U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "PROVISIONING_MANAGER";

static const char PROVISIONING_SERVICE_NAME_PREFIX[] =
    "PROV_";

/*
 * Temporary development Proof of Possession.
 *
 * This value is intentionally not treated as a production secret.
 * A production device should use a device-specific value provisioned during
 * manufacturing or obtained from protected configuration.
 */
static const char PROVISIONING_SECURITY1_POP[] =
    "smartgw-setup";

static const char PROVISIONING_QR_VERSION[] =
    "v1";

static const char PROVISIONING_QR_TRANSPORT[] =
    "ble";

static const char PROVISIONING_QR_JSON_FORMAT[] =
    "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\","
    "\"transport\":\"%s\"}";

/* Static Variables --------------------------------------------------------- */
static portMUX_TYPE s_state_lock =
    portMUX_INITIALIZER_UNLOCKED;

static provisioning_manager_state_t s_state =
    PROVISIONING_MANAGER_STATE_UNINITIALIZED;

static bool s_initializing = false;
static bool s_cleanup_in_progress = false;
static uint32_t s_session_generation = 0U;
static bool s_ble_memory_released = false;
static bool s_ble_memory_releasing = false;
static uint32_t s_ble_memory_release_failure_count = 0U;

static QueueHandle_t s_credentials_queue = NULL;

static provisioning_manager_wifi_credentials_t s_pending_credentials = {0};

static bool s_pending_credentials_valid = false;
static bool s_wifi_handoff_pending = false;
static bool s_late_wifi_handoff_armed = false;
static uint32_t s_late_wifi_handoff_generation = 0U;

static char s_active_service_name
    [PROVISIONING_MANAGER_SERVICE_NAME_BUFFER_SIZE] = {0};

static char s_active_qr_payload
    [PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE] = {0};

static bool s_active_qr_payload_available = false;

static provisioning_manager_progress_callback_t s_progress_callback = NULL;
static void *s_progress_callback_user_data = NULL;

/* Function Prototypes ------------------------------------------------------ */
/**
 * @brief Securely overwrite a buffer that may contain sensitive data.
 *
 * @param[in,out] buffer Buffer to clear.
 * @param[in] size Buffer size in bytes.
 */
static void provisioning_manager_zeroize(
    void *buffer,
    size_t size);

/**
 * @brief Overwrite and reset the retained credential queue.
 *
 * FreeRTOS queue receive/reset operations update queue indexes but do not
 * guarantee erasure of the backing item storage. The queue has length one, so
 * overwriting it with a zero item before reset removes any retained plaintext
 * credential bytes.
 *
 * Call only when no new credential handoff can be produced concurrently.
 *
 * @param[in] credentials_queue Retained length-one credential queue.
 *
 * @return ESP_OK when the backing item and queue state were cleared.
 */
static esp_err_t provisioning_manager_clear_credentials_queue(
    QueueHandle_t credentials_queue);

/**
 * @brief Build a unique BLE service name from the Station MAC address.
 *
 * @param[out] service_name Destination for the null-terminated service name.
 * @param[in] service_name_size Size of the destination buffer in bytes.
 *
 * @return ESP_OK on success, or an ESP-IDF error code on failure.
 */
static esp_err_t provisioning_manager_build_service_name(
    char *service_name,
    size_t service_name_size);

/**
 * @brief Build bounded provisioning JSON from the active service identity.
 *
 * @param[in] service_name Service name passed to the provisioning framework.
 * @param[out] payload Destination for the null-terminated JSON.
 * @param[in] payload_size Destination size in bytes.
 *
 * @return ESP_OK on success, or an error for invalid input or truncation.
 */
static esp_err_t provisioning_manager_build_qr_payload(
    const char *service_name,
    char *payload,
    size_t payload_size);

/**
 * @brief Clear cached service identity while s_state_lock is held.
 */
static void provisioning_manager_clear_active_identity_locked(void);

/**
 * @brief Replace the current lifecycle state atomically.
 *
 * @param[in] state New lifecycle state.
 */
static void provisioning_manager_set_state(
    provisioning_manager_state_t state);

/**
 * @brief Publish copied progress outside the manager critical section.
 *
 * @param[in] progress Progress event to publish.
 * @param[in] last_error Associated ESP-IDF error, or ESP_OK.
 * @param[in] wifi_failure_reason Framework Wi-Fi failure reason, or zero.
 */
static void provisioning_manager_publish_progress(
    provisioning_manager_progress_t progress,
    esp_err_t last_error,
    uint16_t wifi_failure_reason);

/**
 * @brief Publish progress for an already captured session generation.
 *
 * This is used by asynchronous cleanup so a following init cannot relabel
 * the previous session's terminal event.
 */
static void provisioning_manager_publish_progress_for_generation(
    uint32_t session_generation,
    provisioning_manager_progress_t progress,
    esp_err_t last_error,
    uint16_t wifi_failure_reason);

/**
 * @brief De-initialize the framework outside its direct event callback.
 *
 * The upstream manager invokes NETWORK_PROV_END while holding an internal
 * mutex. This one-shot task prevents recursive acquisition of that mutex.
 *
 * @param[in] arg Unused task argument.
 */
static void provisioning_manager_cleanup_task(
    void *arg);

/**
 * @brief Receive lifecycle events directly from the provisioning framework.
 *
 * @param[in] user_data Optional callback context; unused.
 * @param[in] event Provisioning framework event.
 * @param[in] event_data Event-specific payload supplied by the framework.
 */
static void provisioning_manager_event_callback(
    void *user_data,
    network_prov_cb_event_t event,
    void *event_data);

/**
 * @brief Read a thread-safe snapshot of the lifecycle state.
 *
 * @return Current provisioning manager state.
 */
static provisioning_manager_state_t
provisioning_manager_get_state_snapshot(void);

/**
 * @brief Validate and deep-copy framework-owned station credentials.
 *
 * @param[out] destination Application-owned destination structure.
 * @param[in] source Framework-owned station configuration.
 *
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG for invalid input.
 */
static esp_err_t provisioning_manager_copy_wifi_credentials(
    provisioning_manager_wifi_credentials_t *destination,
    const wifi_sta_config_t *source);

/* Private Functions -------------------------------------------------------- */
static void provisioning_manager_zeroize(
    void *buffer,
    size_t size)
{
    volatile uint8_t *byte =
        (volatile uint8_t *)buffer;

    while ((byte != NULL) &&
           (size > 0U))
    {
        *byte = 0U;
        byte++;
        size--;
    }
}

static esp_err_t provisioning_manager_clear_credentials_queue(
    QueueHandle_t credentials_queue)
{
    if (credentials_queue == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    provisioning_manager_wifi_credentials_t zero_credentials = {0};

    const BaseType_t overwrite_ret =
        xQueueOverwrite(
            credentials_queue,
            &zero_credentials);

    provisioning_manager_zeroize(
        &zero_credentials,
        sizeof(zero_credentials));

    if (overwrite_ret != pdPASS)
    {
        return ESP_FAIL;
    }

    if (xQueueReset(credentials_queue) != pdPASS)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t provisioning_manager_copy_wifi_credentials(
    provisioning_manager_wifi_credentials_t *destination,
    const wifi_sta_config_t *source)
{
    if ((destination == NULL) ||
        (source == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        destination,
        0,
        sizeof(*destination));

    const size_t ssid_length =
        strnlen(
            (const char *)source->ssid,
            sizeof(source->ssid));

    const size_t password_length =
        strnlen(
            (const char *)source->password,
            sizeof(source->password));

    /*
     * A 32-byte SSID may not contain a null terminator in the source array.
     * The destination has one extra byte, so it can always be terminated.
     */
    if ((ssid_length == 0U) ||
        (ssid_length >
         PROVISIONING_MANAGER_WIFI_SSID_MAX_LEN))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Open networks use an empty password.
     * WPA/WPA2 passphrases accepted by config_manager contain 8-63 bytes.
     */
    if ((password_length != 0U) &&
        ((password_length < 8U) ||
         (password_length >
          PROVISIONING_MANAGER_WIFI_PASSWORD_MAX_LEN)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(
        destination->ssid,
        source->ssid,
        ssid_length);

    destination->ssid[ssid_length] = '\0';

    memcpy(
        destination->password,
        source->password,
        password_length);

    destination->password[password_length] = '\0';

    return ESP_OK;
}

static esp_err_t provisioning_manager_build_service_name(
    char *service_name,
    size_t service_name_size)
{
    if ((service_name == NULL) ||
        (service_name_size <
         PROVISIONING_MANAGER_SERVICE_NAME_BUFFER_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }

    service_name[0] = '\0';

    uint8_t station_mac[6] = {0};

    esp_err_t ret =
        esp_read_mac(
            station_mac,
            ESP_MAC_WIFI_STA);

    if (ret != ESP_OK)
    {
        return ret;
    }

    int written =
        snprintf(
            service_name,
            service_name_size,
            "%s%02X%02X%02X",
            PROVISIONING_SERVICE_NAME_PREFIX,
            station_mac[3],
            station_mac[4],
            station_mac[5]);

    if ((written < 0) ||
        ((size_t)written >= service_name_size))
    {
        service_name[0] = '\0';

        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static bool provisioning_manager_is_json_token_safe(
    const char *value)
{
    if ((value == NULL) ||
        (value[0] == '\0'))
    {
        return false;
    }

    const unsigned char *character =
        (const unsigned char *)value;

    while (*character != '\0')
    {
        /*
         * The formatter does not escape JSON strings. Restrict onboarding
         * identity tokens so a future device-specific PoP cannot produce
         * malformed JSON or inject another field.
         */
        if ((*character < 0x20U) ||
            (*character > 0x7EU) ||
            (*character == (unsigned char)'"') ||
            (*character == (unsigned char)'\\'))
        {
            return false;
        }

        character++;
    }

    return true;
}

static esp_err_t provisioning_manager_build_qr_payload(
    const char *service_name,
    char *payload,
    size_t payload_size)
{
    if ((service_name == NULL) ||
        (payload == NULL) ||
        (payload_size <
         PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }

    payload[0] = '\0';

    if (!provisioning_manager_is_json_token_safe(
            PROVISIONING_QR_VERSION) ||
        !provisioning_manager_is_json_token_safe(
            service_name) ||
        !provisioning_manager_is_json_token_safe(
            PROVISIONING_SECURITY1_POP) ||
        !provisioning_manager_is_json_token_safe(
            PROVISIONING_QR_TRANSPORT))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int written =
        snprintf(
            payload,
            payload_size,
            PROVISIONING_QR_JSON_FORMAT,
            PROVISIONING_QR_VERSION,
            service_name,
            PROVISIONING_SECURITY1_POP,
            PROVISIONING_QR_TRANSPORT);

    if (written < 0)
    {
        provisioning_manager_zeroize(
            payload,
            payload_size);

        return ESP_FAIL;
    }

    if ((size_t)written >= payload_size)
    {
        provisioning_manager_zeroize(
            payload,
            payload_size);

        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void provisioning_manager_clear_active_identity_locked(void)
{
    provisioning_manager_zeroize(
        s_active_service_name,
        sizeof(s_active_service_name));

    provisioning_manager_zeroize(
        s_active_qr_payload,
        sizeof(s_active_qr_payload));

    s_active_qr_payload_available = false;
}

static void provisioning_manager_set_state(
    provisioning_manager_state_t state)
{
    portENTER_CRITICAL(&s_state_lock);

    s_state = state;

    portEXIT_CRITICAL(&s_state_lock);
}

static void provisioning_manager_publish_progress(
    provisioning_manager_progress_t progress,
    esp_err_t last_error,
    uint16_t wifi_failure_reason)
{
    uint32_t session_generation = 0U;

    portENTER_CRITICAL(&s_state_lock);

    session_generation = s_session_generation;

    portEXIT_CRITICAL(&s_state_lock);

    provisioning_manager_publish_progress_for_generation(
        session_generation,
        progress,
        last_error,
        wifi_failure_reason);
}

static void provisioning_manager_publish_progress_for_generation(
    uint32_t session_generation,
    provisioning_manager_progress_t progress,
    esp_err_t last_error,
    uint16_t wifi_failure_reason)
{
    provisioning_manager_progress_callback_t callback = NULL;
    void *callback_user_data = NULL;

    portENTER_CRITICAL(&s_state_lock);

    callback = s_progress_callback;
    callback_user_data = s_progress_callback_user_data;

    portEXIT_CRITICAL(&s_state_lock);

    if (callback == NULL)
    {
        return;
    }

    const provisioning_manager_progress_status_t status =
    {
        .session_generation = session_generation,
        .progress = progress,
        .last_error = last_error,
        .wifi_failure_reason = wifi_failure_reason,
    };

    callback(
        &status,
        callback_user_data);
}

static void provisioning_manager_cleanup_task(
    void *arg)
{
    (void)arg;
    uint32_t completed_generation = 0U;
    bool late_handoff_retained = false;

    const esp_err_t ret =
        network_prov_mgr_deinit();

    portENTER_CRITICAL(&s_state_lock);

    completed_generation = s_session_generation;

    const bool unverified_credentials_pending =
        s_pending_credentials_valid;

    late_handoff_retained =
        (ret == ESP_OK) &&
        s_late_wifi_handoff_armed &&
        (s_late_wifi_handoff_generation == completed_generation) &&
        unverified_credentials_pending &&
        s_wifi_handoff_pending;

    if (!late_handoff_retained)
    {
        provisioning_manager_zeroize(
            &s_pending_credentials,
            sizeof(s_pending_credentials));

        s_pending_credentials_valid = false;

        /*
         * Cleanup canceled an in-flight, not-yet-verified credential set.
         * A true handoff with no pending copy can instead represent verified
         * credentials already queued for the application; preserve that flag
         * until receive or terminal queue cleanup.
         */
        if (unverified_credentials_pending)
        {
            s_wifi_handoff_pending = false;
        }

        s_late_wifi_handoff_armed = false;
        s_late_wifi_handoff_generation = 0U;
    }

    s_cleanup_in_progress = false;
    provisioning_manager_clear_active_identity_locked();
    s_state =
        (ret == ESP_OK)
            ? PROVISIONING_MANAGER_STATE_STOPPED
            : PROVISIONING_MANAGER_STATE_FAILED;

    portEXIT_CRITICAL(&s_state_lock);

    if (ret == ESP_OK)
    {
        if (late_handoff_retained)
        {
            ESP_LOGI(
                TAG,
                "Retained generation-bound Wi-Fi handoff for bounded "
                "post-stop reconciliation");
        }

        ESP_LOGI(
            TAG,
            "Provisioning manager de-initialized");

        provisioning_manager_publish_progress_for_generation(
            completed_generation,
            PROVISIONING_MANAGER_PROGRESS_STOPPED,
            ESP_OK,
            0U);
    }
    else
    {
        ESP_LOGE(
            TAG,
            "Failed to de-initialize provisioning framework: %s",
            esp_err_to_name(ret));

        provisioning_manager_publish_progress_for_generation(
            completed_generation,
            PROVISIONING_MANAGER_PROGRESS_FAILED,
            ret,
            0U);
    }

    vTaskDelete(NULL);
}

static void provisioning_manager_event_callback(
    void *user_data,
    network_prov_cb_event_t event,
    void *event_data)
{
    (void)user_data;

    switch (event)
    {
        case NETWORK_PROV_INIT:
            ESP_LOGD(
                TAG,
                "Underlying provisioning framework initialized");
            break;

        case NETWORK_PROV_START:
            ESP_LOGD(
                TAG,
                "Underlying provisioning service started");
            break;

        case NETWORK_PROV_WIFI_CRED_RECV:
        {
            const wifi_sta_config_t *received_credentials =
                (const wifi_sta_config_t *)event_data;

            provisioning_manager_wifi_credentials_t credentials_copy = {0};

            esp_err_t copy_ret =
                provisioning_manager_copy_wifi_credentials(
                    &credentials_copy,
                    received_credentials);

            if (copy_ret != ESP_OK)
            {
                portENTER_CRITICAL(&s_state_lock);

                provisioning_manager_zeroize(
                    &s_pending_credentials,
                    sizeof(s_pending_credentials));

                s_pending_credentials_valid = false;
                s_wifi_handoff_pending = false;
                s_late_wifi_handoff_armed = false;
                s_late_wifi_handoff_generation = 0U;

                portEXIT_CRITICAL(&s_state_lock);

                provisioning_manager_zeroize(
                    &credentials_copy,
                    sizeof(credentials_copy));

                ESP_LOGE(
                    TAG,
                    "Received invalid Wi-Fi credentials");

                provisioning_manager_publish_progress(
                    PROVISIONING_MANAGER_PROGRESS_WIFI_CREDENTIAL_FAILED,
                    copy_ret,
                    0U);

                break;
            }

            portENTER_CRITICAL(&s_state_lock);

            provisioning_manager_zeroize(
                &s_pending_credentials,
                sizeof(s_pending_credentials));

            memcpy(
                &s_pending_credentials,
                &credentials_copy,
                sizeof(s_pending_credentials));

            s_pending_credentials_valid = true;
            s_wifi_handoff_pending = true;
            s_late_wifi_handoff_armed = false;
            s_late_wifi_handoff_generation = 0U;

            portEXIT_CRITICAL(&s_state_lock);

            provisioning_manager_zeroize(
                &credentials_copy,
                sizeof(credentials_copy));

            ESP_LOGI(
                TAG,
                "Wi-Fi credentials received; awaiting connection result");

            provisioning_manager_publish_progress(
                PROVISIONING_MANAGER_PROGRESS_CREDENTIAL_RECEIVED,
                ESP_OK,
                0U);

            provisioning_manager_publish_progress(
                PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTING,
                ESP_OK,
                0U);

            break;
        }

        case NETWORK_PROV_WIFI_CRED_FAIL:
        {
            uint16_t wifi_failure_reason = 0U;

            if (event_data != NULL)
            {
                wifi_failure_reason =
                    (uint16_t)(
                        *(const network_prov_wifi_sta_fail_reason_t *)
                            event_data);
            }

            portENTER_CRITICAL(&s_state_lock);

            provisioning_manager_zeroize(
                &s_pending_credentials,
                sizeof(s_pending_credentials));

            s_pending_credentials_valid = false;
            s_wifi_handoff_pending = false;
            s_late_wifi_handoff_armed = false;
            s_late_wifi_handoff_generation = 0U;

            portEXIT_CRITICAL(&s_state_lock);

            ESP_LOGW(
                TAG,
                "Provisioned Wi-Fi connection failed; pending credentials "
                "discarded");

            provisioning_manager_publish_progress(
                PROVISIONING_MANAGER_PROGRESS_WIFI_CREDENTIAL_FAILED,
                ESP_ERR_INVALID_RESPONSE,
                wifi_failure_reason);
            break;
        }

        case NETWORK_PROV_WIFI_CRED_SUCCESS:
        {
            provisioning_manager_wifi_credentials_t credentials_copy = {0};
            QueueHandle_t credentials_queue = NULL;
            bool credentials_ready = false;

            portENTER_CRITICAL(&s_state_lock);

            if (s_pending_credentials_valid)
            {
                memcpy(
                    &credentials_copy,
                    &s_pending_credentials,
                    sizeof(credentials_copy));

                provisioning_manager_zeroize(
                    &s_pending_credentials,
                    sizeof(s_pending_credentials));

                s_pending_credentials_valid = false;
                credentials_ready = true;
            }

            s_wifi_handoff_pending =
                credentials_ready;
            s_late_wifi_handoff_armed = false;
            s_late_wifi_handoff_generation = 0U;

            credentials_queue = s_credentials_queue;

            portEXIT_CRITICAL(&s_state_lock);

            if (!credentials_ready ||
                (credentials_queue == NULL))
            {
                portENTER_CRITICAL(&s_state_lock);

                s_wifi_handoff_pending = false;

                portEXIT_CRITICAL(&s_state_lock);

                provisioning_manager_zeroize(
                    &credentials_copy,
                    sizeof(credentials_copy));

                ESP_LOGE(
                    TAG,
                    "Provisioned connection succeeded but credential "
                    "handoff is unavailable");

                provisioning_manager_publish_progress(
                    PROVISIONING_MANAGER_PROGRESS_FAILED,
                    ESP_ERR_INVALID_STATE,
                    0U);

                break;
            }

            BaseType_t queue_ret =
                xQueueOverwrite(
                    credentials_queue,
                    &credentials_copy);

            provisioning_manager_zeroize(
                &credentials_copy,
                sizeof(credentials_copy));

            if (queue_ret != pdPASS)
            {
                portENTER_CRITICAL(&s_state_lock);

                s_wifi_handoff_pending = false;

                portEXIT_CRITICAL(&s_state_lock);

                ESP_LOGE(
                    TAG,
                    "Failed to queue verified Wi-Fi credentials");

                provisioning_manager_publish_progress(
                    PROVISIONING_MANAGER_PROGRESS_FAILED,
                    ESP_FAIL,
                    0U);

                break;
            }

            ESP_LOGI(
                TAG,
                "Provisioned Wi-Fi connection succeeded; credentials ready "
                "for application handoff");

            provisioning_manager_publish_progress(
                PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTED,
                ESP_OK,
                0U);
            break;
        }

        case NETWORK_PROV_END:
        {
            /*
             * The transport has finished stopping.
             *
             * Schedule framework de-initialization. This releases manager
             * resources and triggers NETWORK_PROV_DEINIT.
             */
            provisioning_manager_set_state(
                PROVISIONING_MANAGER_STATE_STOPPING);

            portENTER_CRITICAL(&s_state_lock);

            provisioning_manager_clear_active_identity_locked();
            s_cleanup_in_progress = true;

            portEXIT_CRITICAL(&s_state_lock);

            provisioning_manager_publish_progress(
                PROVISIONING_MANAGER_PROGRESS_STOPPING,
                ESP_OK,
                0U);

            ESP_LOGI(
                TAG,
                "Provisioning service stopped");

            /*
             * NETWORK_PROV_END is delivered while the framework still owns
             * its internal mutex. Calling network_prov_mgr_deinit() directly
             * here would attempt to take that mutex again and deadlock.
             */
            BaseType_t task_ret =
                xTaskCreate(
                    provisioning_manager_cleanup_task,
                    "prov_cleanup",
                    PROVISIONING_MANAGER_CLEANUP_TASK_STACK_SIZE_BYTES,
                    NULL,
                    PROVISIONING_MANAGER_CLEANUP_TASK_PRIORITY,
                    NULL);

            if (task_ret != pdPASS)
            {
                QueueHandle_t credentials_queue = NULL;

                portENTER_CRITICAL(&s_state_lock);

                provisioning_manager_zeroize(
                    &s_pending_credentials,
                    sizeof(s_pending_credentials));

                s_pending_credentials_valid = false;
                s_wifi_handoff_pending = false;
                s_late_wifi_handoff_armed = false;
                s_late_wifi_handoff_generation = 0U;
                s_cleanup_in_progress = false;
                provisioning_manager_clear_active_identity_locked();
                s_state =
                    PROVISIONING_MANAGER_STATE_FAILED;
                credentials_queue = s_credentials_queue;

                portEXIT_CRITICAL(&s_state_lock);

                const esp_err_t queue_cleanup_ret =
                    provisioning_manager_clear_credentials_queue(
                        credentials_queue);

                ESP_LOGE(
                    TAG,
                    "Failed to create provisioning cleanup task");

                if (queue_cleanup_ret != ESP_OK)
                {
                    ESP_LOGE(
                        TAG,
                        "Failed to clear retained provisioning credential "
                        "queue");
                }

                provisioning_manager_publish_progress(
                    PROVISIONING_MANAGER_PROGRESS_FAILED,
                    ESP_ERR_NO_MEM,
                    0U);
            }

            break;
        }

        case NETWORK_PROV_DEINIT:
            /*
             * The cleanup task publishes STOPPED only after
             * network_prov_mgr_deinit() has returned. Keeping STOPPING here
             * prevents a new session from racing the old cleanup task.
             */
            ESP_LOGD(
                TAG,
                "Underlying provisioning framework de-initialized");
            break;

        default:
            break;
    }
}

static provisioning_manager_state_t
provisioning_manager_get_state_snapshot(void)
{
    provisioning_manager_state_t state;

    portENTER_CRITICAL(&s_state_lock);

    state = s_state;

    portEXIT_CRITICAL(&s_state_lock);

    return state;
}

/* Functions ---------------------------------------------------------------- */
esp_err_t provisioning_manager_register_progress_callback(
    provisioning_manager_progress_callback_t callback,
    void *user_data)
{
    portENTER_CRITICAL(&s_state_lock);

    if ((callback != NULL) &&
        (s_progress_callback != NULL) &&
        ((s_progress_callback != callback) ||
         (s_progress_callback_user_data != user_data)))
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    s_progress_callback = callback;
    s_progress_callback_user_data =
        (callback != NULL)
            ? user_data
            : NULL;

    portEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
}

esp_err_t provisioning_manager_init(
    uint32_t session_generation)
{
    provisioning_manager_state_t current_state;
    QueueHandle_t credentials_queue = NULL;
    bool credentials_queue_created = false;

    if (session_generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);

    current_state = s_state;

    if (s_initializing)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    if ((current_state ==
         PROVISIONING_MANAGER_STATE_READY) &&
        (s_session_generation ==
         session_generation))
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_OK;
    }

    /*
     * A STOPPED generation may still own a deliberately retained late
     * handoff or a verified queue item. Require its coordinator to confirm,
     * consume, or securely discard that handoff before a new generation can
     * overwrite the session identity. This fails closed instead of silently
     * dropping credentials at the retry boundary.
     */
    if ((current_state ==
         PROVISIONING_MANAGER_STATE_STOPPED) &&
        (s_late_wifi_handoff_armed ||
         s_pending_credentials_valid ||
         s_wifi_handoff_pending))
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    if (((current_state !=
          PROVISIONING_MANAGER_STATE_UNINITIALIZED) &&
        (current_state !=
          PROVISIONING_MANAGER_STATE_STOPPED)) ||
        s_cleanup_in_progress ||
        s_ble_memory_releasing ||
        s_ble_memory_released)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    s_initializing = true;
    s_session_generation = session_generation;

    provisioning_manager_zeroize(
        &s_pending_credentials,
        sizeof(s_pending_credentials));

    s_pending_credentials_valid = false;
    s_wifi_handoff_pending = false;
    s_late_wifi_handoff_armed = false;
    s_late_wifi_handoff_generation = 0U;
    provisioning_manager_clear_active_identity_locked();
    credentials_queue = s_credentials_queue;

    portEXIT_CRITICAL(&s_state_lock);

    network_prov_mgr_config_t config =
    {
        .network_prov_wifi_conn_cfg =
        {
            .wifi_conn_attempts =
                PROVISIONING_MANAGER_WIFI_CONNECTION_ATTEMPTS,
        },

        .scheme =
            network_prov_scheme_ble,

        /*
         * ESP32-S3 is BLE-only. Retain BLE across intermediate manager
         * deinitialization and release it explicitly after the retry envelope.
         */
        .scheme_event_handler =
            NETWORK_PROV_EVENT_HANDLER_NONE,

        .app_event_handler =
        {
            .event_cb =
                provisioning_manager_event_callback,

            .user_data =
                NULL,
        },
    };

    if (credentials_queue == NULL)
    {
        credentials_queue =
            xQueueCreate(
            PROVISIONING_MANAGER_CREDENTIAL_QUEUE_LENGTH,
            sizeof(provisioning_manager_wifi_credentials_t));

        credentials_queue_created =
            (credentials_queue != NULL);
    }
    else if (provisioning_manager_clear_credentials_queue(
                 credentials_queue) != ESP_OK)
    {
        portENTER_CRITICAL(&s_state_lock);

        s_initializing = false;
        s_state = PROVISIONING_MANAGER_STATE_FAILED;

        portEXIT_CRITICAL(&s_state_lock);

        ESP_LOGE(
            TAG,
            "Failed to reset provisioning credential queue");

        provisioning_manager_publish_progress(
            PROVISIONING_MANAGER_PROGRESS_FAILED,
            ESP_FAIL,
            0U);

        return ESP_FAIL;
    }

    if (credentials_queue == NULL)
    {
        portENTER_CRITICAL(&s_state_lock);

        s_initializing = false;
        s_state = PROVISIONING_MANAGER_STATE_FAILED;

        portEXIT_CRITICAL(&s_state_lock);

        ESP_LOGE(
            TAG,
            "Failed to create provisioning credential queue");

        provisioning_manager_publish_progress(
            PROVISIONING_MANAGER_PROGRESS_FAILED,
            ESP_ERR_NO_MEM,
            0U);

        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_state_lock);

    s_credentials_queue = credentials_queue;

    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t ret =
        network_prov_mgr_init(config);

    if ((ret != ESP_OK) &&
        credentials_queue_created)
    {
        vQueueDelete(credentials_queue);

        portENTER_CRITICAL(&s_state_lock);

        if (s_credentials_queue ==
            credentials_queue)
        {
            s_credentials_queue = NULL;
        }

        portEXIT_CRITICAL(&s_state_lock);
    }

    portENTER_CRITICAL(&s_state_lock);

    s_initializing = false;

    if (ret == ESP_OK)
    {
        s_state =
            PROVISIONING_MANAGER_STATE_READY;
    }
    else
    {
        s_state =
            PROVISIONING_MANAGER_STATE_FAILED;
    }

    portEXIT_CRITICAL(&s_state_lock);

    if (ret != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize provisioning framework: %s",
            esp_err_to_name(ret));

        provisioning_manager_publish_progress(
            PROVISIONING_MANAGER_PROGRESS_FAILED,
            ret,
            0U);

        return ret;
    }

    ESP_LOGI(
        TAG,
        "Provisioning manager initialized");

    return ESP_OK;
}

esp_err_t provisioning_manager_start(void)
{
    /*
     * Claim READY -> STARTING atomically so concurrent callers cannot start
     * the provisioning service more than once.
     */
    portENTER_CRITICAL(&s_state_lock);

    if (s_state != PROVISIONING_MANAGER_STATE_READY)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    s_state =
        PROVISIONING_MANAGER_STATE_STARTING;

    portEXIT_CRITICAL(&s_state_lock);

    provisioning_manager_publish_progress(
        PROVISIONING_MANAGER_PROGRESS_STARTING,
        ESP_OK,
        0U);

    char service_name
        [PROVISIONING_MANAGER_SERVICE_NAME_BUFFER_SIZE] =
        {0};

    esp_err_t ret =
        provisioning_manager_build_service_name(
            service_name,
            sizeof(service_name));

    if (ret != ESP_OK)
    {
        provisioning_manager_set_state(
            PROVISIONING_MANAGER_STATE_FAILED);

        ESP_LOGE(
            TAG,
            "Failed to build provisioning service name: %s",
            esp_err_to_name(ret));

        provisioning_manager_publish_progress(
            PROVISIONING_MANAGER_PROGRESS_FAILED,
            ret,
            0U);

        return ret;
    }

    /*
     * Security 1 uses a Proof of Possession string.
     *
     * The PoP has static storage duration because the provisioning framework
     * requires security parameters to remain valid while the service runs.
     */
    const network_prov_security1_params_t *security_params =
        PROVISIONING_SECURITY1_POP;

    /*
     * Security 1 logs session material at INFO level in the upstream
     * implementation. Preserve warnings while keeping that material out of
     * normal serial output.
     */
    esp_log_level_set(
        "security1",
        ESP_LOG_WARN);

    ret =
        network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1,
            (const void *)security_params,
            service_name,
            NULL);

    if (ret != ESP_OK)
    {
        uint32_t failed_generation = 0U;

        portENTER_CRITICAL(&s_state_lock);

        failed_generation = s_session_generation;
        provisioning_manager_clear_active_identity_locked();
        s_state = PROVISIONING_MANAGER_STATE_STOPPING;

        portEXIT_CRITICAL(&s_state_lock);

        ESP_LOGE(
            TAG,
            "Failed to start BLE provisioning: %s",
            esp_err_to_name(ret));

        provisioning_manager_publish_progress_for_generation(
            failed_generation,
            PROVISIONING_MANAGER_PROGRESS_STOPPING,
            ESP_OK,
            0U);

        const esp_err_t cleanup_ret =
            network_prov_mgr_deinit();

        portENTER_CRITICAL(&s_state_lock);

        provisioning_manager_zeroize(
            &s_pending_credentials,
            sizeof(s_pending_credentials));

        s_pending_credentials_valid = false;
        s_wifi_handoff_pending = false;
        s_late_wifi_handoff_armed = false;
        s_late_wifi_handoff_generation = 0U;
        provisioning_manager_clear_active_identity_locked();
        s_state =
            (cleanup_ret == ESP_OK)
                ? PROVISIONING_MANAGER_STATE_STOPPED
                : PROVISIONING_MANAGER_STATE_FAILED;

        portEXIT_CRITICAL(&s_state_lock);

        if (cleanup_ret == ESP_OK)
        {
            provisioning_manager_publish_progress_for_generation(
                failed_generation,
                PROVISIONING_MANAGER_PROGRESS_STOPPED,
                ESP_OK,
                0U);

            return ret;
        }

        ESP_LOGE(
            TAG,
            "Failed to clean provisioning framework after start error: %s",
            esp_err_to_name(cleanup_ret));

        provisioning_manager_publish_progress_for_generation(
            failed_generation,
            PROVISIONING_MANAGER_PROGRESS_FAILED,
            ret,
            0U);

        /*
         * Cleanup failure keeps the manager in FAILED so the framework is
         * never reused unsafely. Return the original start error because it is
         * the first operation that failed; cleanup_ret remains visible in the
         * diagnostic above.
         */
        return ret;
    }

    char qr_payload
        [PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE] =
        {0};

    const esp_err_t qr_ret =
        provisioning_manager_build_qr_payload(
            service_name,
            qr_payload,
            sizeof(qr_payload));

    portENTER_CRITICAL(&s_state_lock);

    provisioning_manager_clear_active_identity_locked();

    memcpy(
        s_active_service_name,
        service_name,
        sizeof(s_active_service_name));

    if (qr_ret == ESP_OK)
    {
        memcpy(
            s_active_qr_payload,
            qr_payload,
            sizeof(s_active_qr_payload));

        s_active_qr_payload_available = true;
    }

    s_state = PROVISIONING_MANAGER_STATE_ACTIVE;

    portEXIT_CRITICAL(&s_state_lock);

    provisioning_manager_zeroize(
        qr_payload,
        sizeof(qr_payload));

    ESP_LOGI(
        TAG,
        "BLE provisioning active with service name: %s",
        service_name);

    if (qr_ret != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "BLE provisioning QR payload is unavailable: %s",
            esp_err_to_name(qr_ret));
    }

    provisioning_manager_publish_progress(
        PROVISIONING_MANAGER_PROGRESS_WAITING_FOR_PHONE,
        ESP_OK,
        0U);

    return ESP_OK;
}

esp_err_t provisioning_manager_get_qr_payload(
    uint32_t session_generation,
    char *payload,
    size_t payload_size)
{
    if ((session_generation == 0U) ||
        (payload == NULL) ||
        (payload_size <
         PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }

    payload[0] = '\0';

    portENTER_CRITICAL(&s_state_lock);

    if ((s_state != PROVISIONING_MANAGER_STATE_ACTIVE) ||
        (s_session_generation != session_generation) ||
        !s_active_qr_payload_available)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    memcpy(
        payload,
        s_active_qr_payload,
        sizeof(s_active_qr_payload));

    portEXIT_CRITICAL(&s_state_lock);

    payload[
        PROVISIONING_MANAGER_QR_PAYLOAD_BUFFER_SIZE - 1U] =
        '\0';

    return ESP_OK;
}

esp_err_t provisioning_manager_stop(void)
{
    /*
     * Claim the ACTIVE -> STOPPING transition before asking the framework
     * to stop. This prevents multiple callers from issuing concurrent stop
     * requests.
     */
    portENTER_CRITICAL(&s_state_lock);

    if (s_state !=
        PROVISIONING_MANAGER_STATE_ACTIVE)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    s_state =
        PROVISIONING_MANAGER_STATE_STOPPING;

    provisioning_manager_clear_active_identity_locked();

    portEXIT_CRITICAL(&s_state_lock);

    provisioning_manager_publish_progress(
        PROVISIONING_MANAGER_PROGRESS_STOPPING,
        ESP_OK,
        0U);

    /*
     * This API only initiates shutdown. Actual transport cleanup happens
     * asynchronously and NETWORK_PROV_END is emitted afterward.
     */
    network_prov_mgr_stop_provisioning();

    ESP_LOGI(
        TAG,
        "Provisioning stop requested");

    return ESP_OK;
}

esp_err_t provisioning_manager_release_ble_memory(void)
{
    QueueHandle_t credentials_queue = NULL;

    portENTER_CRITICAL(&s_state_lock);

    if (s_ble_memory_released)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_OK;
    }

    if ((s_state !=
         PROVISIONING_MANAGER_STATE_STOPPED) ||
        s_initializing ||
        s_cleanup_in_progress ||
        s_ble_memory_releasing)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    s_ble_memory_releasing = true;

    if (s_pending_credentials_valid)
    {
        provisioning_manager_zeroize(
            &s_pending_credentials,
            sizeof(s_pending_credentials));

        s_pending_credentials_valid = false;
        s_wifi_handoff_pending = false;
    }

    s_late_wifi_handoff_armed = false;
    s_late_wifi_handoff_generation = 0U;
    credentials_queue = s_credentials_queue;

    portEXIT_CRITICAL(&s_state_lock);

    const esp_err_t queue_cleanup_ret =
        provisioning_manager_clear_credentials_queue(
            credentials_queue);

    if (queue_cleanup_ret != ESP_OK)
    {
        portENTER_CRITICAL(&s_state_lock);

        s_ble_memory_releasing = false;

        portEXIT_CRITICAL(&s_state_lock);

        ESP_LOGE(
            TAG,
            "Failed to clear terminal provisioning credential queue");

        return queue_cleanup_ret;
    }

    portENTER_CRITICAL(&s_state_lock);

    s_wifi_handoff_pending = false;
    s_late_wifi_handoff_armed = false;
    s_late_wifi_handoff_generation = 0U;

    portEXIT_CRITICAL(&s_state_lock);

    const esp_err_t ret =
        esp_bt_mem_release(
            ESP_BT_MODE_BLE);

    if ((ret != ESP_OK) &&
        (ret != ESP_ERR_NOT_FOUND))
    {
        uint32_t failure_count = 0U;

        portENTER_CRITICAL(&s_state_lock);

        s_ble_memory_releasing = false;
        s_ble_memory_release_failure_count++;
        failure_count =
            s_ble_memory_release_failure_count;

        portEXIT_CRITICAL(&s_state_lock);

        ESP_LOGW(
            TAG,
            "Terminal BLE memory release unavailable: %s "
            "(failure_count=%lu)",
            esp_err_to_name(ret),
            (unsigned long)failure_count);

        return ret;
    }

    portENTER_CRITICAL(&s_state_lock);

    s_ble_memory_released = true;
    s_ble_memory_releasing = false;

    portEXIT_CRITICAL(&s_state_lock);

    if (ret == ESP_ERR_NOT_FOUND)
    {
        ESP_LOGD(
            TAG,
            "Terminal BLE memory was already released");
    }
    else
    {
        ESP_LOGI(
            TAG,
            "Terminal BLE memory released");
    }

    return ESP_OK;
}

esp_err_t provisioning_manager_get_state(
    provisioning_manager_state_t *state)
{
    ESP_RETURN_ON_FALSE(
        state != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "State is NULL"
    );

    *state =
        provisioning_manager_get_state_snapshot();

    return ESP_OK;
}

esp_err_t provisioning_manager_is_wifi_handoff_pending(
    bool *handoff_pending)
{
    if (handoff_pending == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);

    if (s_credentials_queue == NULL)
    {
        portEXIT_CRITICAL(&s_state_lock);

        return ESP_ERR_INVALID_STATE;
    }

    *handoff_pending =
        s_wifi_handoff_pending;

    portEXIT_CRITICAL(&s_state_lock);

    return ESP_OK;
}

esp_err_t provisioning_manager_arm_late_wifi_handoff(
    uint32_t session_generation)
{
    if (session_generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bool newly_armed = false;

    portENTER_CRITICAL(&s_state_lock);

    const bool lifecycle_reconcilable =
        (s_state == PROVISIONING_MANAGER_STATE_ACTIVE) ||
        (s_state == PROVISIONING_MANAGER_STATE_STOPPING) ||
        (s_state == PROVISIONING_MANAGER_STATE_STOPPED);

    if (!lifecycle_reconcilable ||
        (s_session_generation != session_generation) ||
        (s_credentials_queue == NULL) ||
        !s_wifi_handoff_pending)
    {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (s_pending_credentials_valid)
    {
        const bool stopped_without_existing_retention =
            (s_state == PROVISIONING_MANAGER_STATE_STOPPED) &&
            (!s_late_wifi_handoff_armed ||
             (s_late_wifi_handoff_generation != session_generation));

        const bool conflicting_retention =
            s_late_wifi_handoff_armed &&
            (s_late_wifi_handoff_generation != session_generation);

        if (stopped_without_existing_retention ||
            conflicting_retention)
        {
            portEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_INVALID_STATE;
        }

        if (!s_late_wifi_handoff_armed)
        {
            s_late_wifi_handoff_armed = true;
            s_late_wifi_handoff_generation = session_generation;
            newly_armed = true;
        }
    }
    else
    {
        /*
         * A framework success callback may have moved the same handoff to the
         * verified queue between the coordinator's final non-blocking receive
         * and this call. Treat that boundary as reconcilable; no unverified
         * copy needs retention.
         */
        s_late_wifi_handoff_armed = false;
        s_late_wifi_handoff_generation = 0U;
    }

    portEXIT_CRITICAL(&s_state_lock);

    if (newly_armed)
    {
        ESP_LOGI(
            TAG,
            "Armed bounded post-stop Wi-Fi handoff reconciliation");
    }

    return ESP_OK;
}

esp_err_t provisioning_manager_confirm_late_wifi_handoff(
    uint32_t session_generation,
    const char *connected_ssid)
{
    if ((session_generation == 0U) ||
        (connected_ssid == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t connected_ssid_length =
        strnlen(
            connected_ssid,
            PROVISIONING_MANAGER_WIFI_SSID_BUFFER_SIZE);

    if ((connected_ssid_length == 0U) ||
        (connected_ssid_length >
         PROVISIONING_MANAGER_WIFI_SSID_MAX_LEN))
    {
        return ESP_ERR_INVALID_ARG;
    }

    provisioning_manager_wifi_credentials_t credentials_copy = {0};
    QueueHandle_t credentials_queue = NULL;

    portENTER_CRITICAL(&s_state_lock);

    if ((s_state != PROVISIONING_MANAGER_STATE_STOPPED) ||
        (s_session_generation != session_generation) ||
        !s_late_wifi_handoff_armed ||
        (s_late_wifi_handoff_generation != session_generation) ||
        !s_pending_credentials_valid ||
        !s_wifi_handoff_pending ||
        (s_credentials_queue == NULL))
    {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(
            s_pending_credentials.ssid,
            connected_ssid) != 0)
    {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memcpy(
        &credentials_copy,
        &s_pending_credentials,
        sizeof(credentials_copy));

    provisioning_manager_zeroize(
        &s_pending_credentials,
        sizeof(s_pending_credentials));

    s_pending_credentials_valid = false;
    s_late_wifi_handoff_armed = false;
    s_late_wifi_handoff_generation = 0U;
    credentials_queue = s_credentials_queue;

    portEXIT_CRITICAL(&s_state_lock);

    const BaseType_t queue_ret =
        xQueueOverwrite(
            credentials_queue,
            &credentials_copy);

    provisioning_manager_zeroize(
        &credentials_copy,
        sizeof(credentials_copy));

    if (queue_ret != pdPASS)
    {
        portENTER_CRITICAL(&s_state_lock);
        s_wifi_handoff_pending = false;
        portEXIT_CRITICAL(&s_state_lock);

        ESP_LOGE(
            TAG,
            "Failed to queue late verified Wi-Fi credentials");

        provisioning_manager_publish_progress_for_generation(
            session_generation,
            PROVISIONING_MANAGER_PROGRESS_FAILED,
            ESP_FAIL,
            0U);

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Late provisioning handoff verified by Station IPv4 state");

    provisioning_manager_publish_progress_for_generation(
        session_generation,
        PROVISIONING_MANAGER_PROGRESS_WIFI_CONNECTED,
        ESP_OK,
        0U);

    return ESP_OK;
}

esp_err_t provisioning_manager_discard_late_wifi_handoff(
    uint32_t session_generation)
{
    if (session_generation == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_state_lock);

    if ((s_session_generation != session_generation) ||
        !s_late_wifi_handoff_armed ||
        (s_late_wifi_handoff_generation != session_generation) ||
        !s_pending_credentials_valid)
    {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    provisioning_manager_zeroize(
        &s_pending_credentials,
        sizeof(s_pending_credentials));

    s_pending_credentials_valid = false;
    s_wifi_handoff_pending = false;
    s_late_wifi_handoff_armed = false;
    s_late_wifi_handoff_generation = 0U;

    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(
        TAG,
        "Discarded retained late Wi-Fi handoff");

    return ESP_OK;
}

esp_err_t provisioning_manager_receive_wifi_credentials(
    provisioning_manager_wifi_credentials_t *credentials,
    uint32_t timeout_ms)
{
    if (credentials == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        credentials,
        0,
        sizeof(*credentials));

    QueueHandle_t credentials_queue =
        s_credentials_queue;

    if (credentials_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t receive_ret =
        xQueueReceive(
            credentials_queue,
            credentials,
            pdMS_TO_TICKS(timeout_ms));

    if (receive_ret != pdTRUE)
    {
        memset(
            credentials,
            0,
            sizeof(*credentials));

        return ESP_ERR_TIMEOUT;
    }

    const esp_err_t queue_cleanup_ret =
        provisioning_manager_clear_credentials_queue(
            credentials_queue);

    if (queue_cleanup_ret != ESP_OK)
    {
        provisioning_manager_zeroize(
            credentials,
            sizeof(*credentials));

        portENTER_CRITICAL(&s_state_lock);

        s_wifi_handoff_pending = false;
        s_late_wifi_handoff_armed = false;
        s_late_wifi_handoff_generation = 0U;

        portEXIT_CRITICAL(&s_state_lock);

        ESP_LOGE(
            TAG,
            "Failed to clear delivered credential queue storage");

        return queue_cleanup_ret;
    }

    portENTER_CRITICAL(&s_state_lock);

    s_wifi_handoff_pending = false;
    s_late_wifi_handoff_armed = false;
    s_late_wifi_handoff_generation = 0U;

    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGD(
        TAG,
        "Verified Wi-Fi credentials delivered to application");

    return ESP_OK;
}
