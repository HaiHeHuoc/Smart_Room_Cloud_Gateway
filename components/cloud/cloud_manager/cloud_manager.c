/* Includes ----------------------------------------------------------------- */
#include "cloud_manager.h"
#include "cloud_telemetry_json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#include "firebase_auth.h"

#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"


/* Macros ------------------------------------------------------------------- */
#define CLOUD_MANAGER_TASK_NAME                    "cloud_manager"
#define CLOUD_MANAGER_TASK_STACK_SIZE              12288U
#define CLOUD_MANAGER_TASK_PRIORITY                4U

#define CLOUD_MANAGER_TELEMETRY_QUEUE_LENGTH       1U

#define CLOUD_MANAGER_HTTP_TIMEOUT_MS              15000U
#define CLOUD_MANAGER_URL_BUFFER_SIZE              256U
#define CLOUD_MANAGER_AUTH_URL_BUFFER_SIZE          \
    (CLOUD_MANAGER_URL_BUFFER_SIZE +                \
     FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE + 64U)
#define CLOUD_MANAGER_HTTP_TX_BUFFER_SIZE            \
    (CLOUD_MANAGER_AUTH_URL_BUFFER_SIZE + 256U)

#define CLOUD_MANAGER_MIN_PUBLISH_PERIOD_MS        1000U

#define CLOUD_MANAGER_RETRY_INITIAL_MS             5000U
#define CLOUD_MANAGER_RETRY_MAX_MS                 60000U
#define CLOUD_MANAGER_MUTEX_TIMEOUT_MS             100U

#define CLOUD_MANAGER_NOTIFY_TELEMETRY_AVAILABLE   (1UL << 0)
#define CLOUD_MANAGER_NOTIFY_NETWORK_CHANGED       (1UL << 1)
#define CLOUD_MANAGER_NOTIFY_ALL                    \
    (CLOUD_MANAGER_NOTIFY_TELEMETRY_AVAILABLE |     \
     CLOUD_MANAGER_NOTIFY_NETWORK_CHANGED)

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "CLOUD_MANAGER";

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    CLOUD_ATTEMPT_SUCCESS = 0,
    CLOUD_ATTEMPT_WAIT_FOR_NETWORK,
    CLOUD_ATTEMPT_RETRYABLE_TRANSPORT,
    CLOUD_ATTEMPT_RETRYABLE_HTTP,
    CLOUD_ATTEMPT_REAUTH_REQUIRED,
    CLOUD_ATTEMPT_AUTH_FATAL,
    CLOUD_ATTEMPT_NONRETRYABLE_CONFIG,
    CLOUD_ATTEMPT_NONRETRYABLE_INTERNAL,
} cloud_attempt_result_t;

typedef struct
{
    cloud_attempt_result_t result;
    esp_err_t error;
    int http_status;
    uint32_t auth_generation;
} cloud_attempt_outcome_t;

typedef struct
{
    bool has_ipv4_address;
    uint32_t epoch;
} cloud_network_snapshot_t;

/* Static Variables --------------------------------------------------------- */
static cloud_manager_config_t s_config;

static char s_firebase_latest_url[
    CLOUD_MANAGER_URL_BUFFER_SIZE];

EXT_RAM_BSS_ATTR static char s_firebase_id_token[
    FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE];

EXT_RAM_BSS_ATTR static char s_authenticated_url[
    CLOUD_MANAGER_AUTH_URL_BUFFER_SIZE];

/* Static because the reusable HTTP client retains the request-body pointer. */
EXT_RAM_BSS_ATTR static char s_telemetry_payload[
    CLOUD_TELEMETRY_JSON_BUFFER_SIZE];

/* Owned exclusively by the cloud task after cloud_manager_start(). */
static esp_http_client_handle_t s_http_client;

EXT_RAM_BSS_ATTR static char s_http_client_url[
    CLOUD_MANAGER_AUTH_URL_BUFFER_SIZE];

static uint32_t s_http_client_network_epoch;
static uint32_t s_http_client_auth_generation;

static QueueHandle_t s_telemetry_queue;
static TaskHandle_t s_cloud_task_handle;

static bool s_is_initialized;
static bool s_is_started;

/*
 * Wi-Fi callbacks and telemetry producers use this short critical section
 * only to retain latest network state and capture the optional task handle.
 */
static portMUX_TYPE s_signal_lock =
    portMUX_INITIALIZER_UNLOCKED;
static bool s_network_has_ipv4_address;
static uint32_t s_network_epoch;

static cloud_manager_status_t s_status;
static SemaphoreHandle_t s_status_mutex;
static cloud_manager_status_callback_t s_status_callback;
static void *s_status_callback_user_data;

/* Function Prototypes ------------------------------------------------------ */
static int64_t cloud_manager_get_time_ms(void);

static void cloud_manager_zeroize(
    void *buffer,
    size_t buffer_size);

static TickType_t cloud_manager_ms_to_ticks_nonzero(
    uint32_t delay_ms);

static TickType_t cloud_manager_delay_remaining(
    TickType_t delay_start,
    TickType_t delay_duration);

static void cloud_manager_wait_for_notification(
    TickType_t timeout_ticks);

static void cloud_manager_wake_task(
    uint32_t notification_bits);

static cloud_network_snapshot_t
cloud_manager_get_network_snapshot(void);

static void cloud_manager_notify_status_changed(void);

static void cloud_manager_set_state(
    cloud_manager_state_t new_state);

static void cloud_manager_record_attempt_start(void);

static void cloud_manager_record_upload_success(
    int http_status);

static void cloud_manager_record_upload_failure(
    const cloud_attempt_outcome_t *outcome,
    cloud_manager_state_t failure_state,
    uint32_t retry_delay_ms);

static void cloud_manager_record_http_client_reset(void);

static void cloud_manager_record_network_epoch(
    uint32_t network_epoch);

static void cloud_manager_record_auth_recovery(void);

static bool cloud_manager_is_retryable_http_error(
    int http_status);

static bool cloud_manager_is_configuration_error(
    esp_err_t error);

static cloud_manager_failure_class_t
cloud_manager_failure_class_from_attempt(
    cloud_attempt_result_t result);

static const char *cloud_manager_failure_class_to_string(
    cloud_manager_failure_class_t failure_class);

static cloud_attempt_result_t
cloud_manager_classify_auth_failure(
    esp_err_t error,
    int *http_status_out);

static esp_err_t cloud_manager_build_authenticated_url(
    uint32_t *auth_generation_out);

static void cloud_manager_reset_http_client(
    const char *reason,
    bool clear_active_identity);

static esp_err_t cloud_manager_prepare_http_client(
    uint32_t network_epoch,
    uint32_t auth_generation);

static void cloud_manager_log_transport_failure(
    esp_err_t error);

static cloud_attempt_outcome_t
cloud_manager_publish_telemetry(
    const cloud_sensor_telemetry_t *telemetry);

static uint32_t cloud_manager_next_retry_delay(
    uint32_t current_delay_ms);

static void cloud_manager_task(
    void *argument);

/* Static Functions --------------------------------------------------------- */
static int64_t cloud_manager_get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void cloud_manager_zeroize(
    void *buffer,
    size_t buffer_size)
{
    volatile unsigned char *cursor =
        (volatile unsigned char *)buffer;

    while ((cursor != NULL) &&
           (buffer_size > 0U))
    {
        *cursor = 0U;
        cursor++;
        buffer_size--;
    }
}

static TickType_t cloud_manager_ms_to_ticks_nonzero(
    uint32_t delay_ms)
{
    TickType_t delay_ticks =
        pdMS_TO_TICKS(delay_ms);

    return
        (delay_ticks == 0U)
            ? 1U
            : delay_ticks;
}

static TickType_t cloud_manager_delay_remaining(
    TickType_t delay_start,
    TickType_t delay_duration)
{
    const TickType_t elapsed =
        xTaskGetTickCount() - delay_start;

    return
        (elapsed >= delay_duration)
            ? 0U
            : delay_duration - elapsed;
}

static void cloud_manager_wait_for_notification(
    TickType_t timeout_ticks)
{
    uint32_t notification_bits = 0U;

    (void)xTaskNotifyWait(
        0U,
        CLOUD_MANAGER_NOTIFY_ALL,
        &notification_bits,
        timeout_ticks);
}

static void cloud_manager_wake_task(
    uint32_t notification_bits)
{
    TaskHandle_t task_handle = NULL;

    portENTER_CRITICAL(&s_signal_lock);
    task_handle = s_cloud_task_handle;
    portEXIT_CRITICAL(&s_signal_lock);

    if (task_handle != NULL)
    {
        (void)xTaskNotify(
            task_handle,
            notification_bits,
            eSetBits);
    }
}

static cloud_network_snapshot_t
cloud_manager_get_network_snapshot(void)
{
    cloud_network_snapshot_t snapshot;

    portENTER_CRITICAL(&s_signal_lock);
    snapshot.has_ipv4_address =
        s_network_has_ipv4_address;
    snapshot.epoch = s_network_epoch;
    portEXIT_CRITICAL(&s_signal_lock);

    return snapshot;
}

static void cloud_manager_notify_status_changed(void)
{
    if (s_status_mutex == NULL)
    {
        return;
    }

    cloud_manager_status_t status_snapshot;
    cloud_manager_status_callback_t callback = NULL;
    void *callback_user_data = NULL;

    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    status_snapshot = s_status;
    callback = s_status_callback;
    callback_user_data = s_status_callback_user_data;

    xSemaphoreGive(s_status_mutex);

    if (callback != NULL)
    {
        callback(
            &status_snapshot,
            callback_user_data);
    }
}

static void cloud_manager_set_state(
    cloud_manager_state_t new_state)
{
    if (s_status_mutex == NULL)
    {
        return;
    }

    cloud_manager_state_t old_state;

    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    old_state = s_status.state;
    s_status.state = new_state;

    xSemaphoreGive(s_status_mutex);

    if (old_state != new_state)
    {
        ESP_LOGD(
            TAG,
            "Cloud state changed: %d -> %d",
            old_state,
            new_state);

        cloud_manager_notify_status_changed();
    }
}

static void cloud_manager_record_attempt_start(void)
{
    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.last_attempt_time_ms =
        cloud_manager_get_time_ms();

    xSemaphoreGive(s_status_mutex);
}

static void cloud_manager_record_upload_success(
    int http_status)
{
    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.state =
        CLOUD_MANAGER_STATE_ONLINE;
    s_status.last_error = ESP_OK;
    s_status.last_http_status = http_status;
    s_status.last_failure_class =
        CLOUD_MANAGER_FAILURE_NONE;

    s_status.successful_upload_count++;
    s_status.consecutive_failure_count = 0U;
    s_status.current_retry_delay_ms =
        CLOUD_MANAGER_RETRY_INITIAL_MS;
    s_status.last_success_time_ms =
        cloud_manager_get_time_ms();

    xSemaphoreGive(s_status_mutex);

    cloud_manager_notify_status_changed();
}

static void cloud_manager_record_upload_failure(
    const cloud_attempt_outcome_t *outcome,
    cloud_manager_state_t failure_state,
    uint32_t retry_delay_ms)
{
    if ((outcome == NULL) ||
        (xSemaphoreTake(
             s_status_mutex,
             portMAX_DELAY) != pdTRUE))
    {
        return;
    }

    s_status.state = failure_state;
    s_status.last_error = outcome->error;
    s_status.last_http_status =
        outcome->http_status;
    s_status.last_failure_class =
        cloud_manager_failure_class_from_attempt(
            outcome->result);

    s_status.failed_upload_count++;
    s_status.consecutive_failure_count++;
    s_status.current_retry_delay_ms =
        retry_delay_ms;

    xSemaphoreGive(s_status_mutex);

    cloud_manager_notify_status_changed();
}

static void cloud_manager_record_http_client_reset(void)
{
    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.http_client_reset_count++;

    xSemaphoreGive(s_status_mutex);
}

static void cloud_manager_record_network_epoch(
    uint32_t network_epoch)
{
    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.network_epoch = network_epoch;

    xSemaphoreGive(s_status_mutex);
}

static void cloud_manager_record_auth_recovery(void)
{
    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.auth_recovery_count++;

    xSemaphoreGive(s_status_mutex);
}

static bool cloud_manager_is_retryable_http_error(
    int http_status)
{
    return
        (http_status == 408) ||
        (http_status == 429) ||
        (http_status >= 500);
}

static bool cloud_manager_is_configuration_error(
    esp_err_t error)
{
    return
        (error == ESP_ERR_INVALID_ARG) ||
        (error == ESP_ERR_INVALID_SIZE);
}

static cloud_manager_failure_class_t
cloud_manager_failure_class_from_attempt(
    cloud_attempt_result_t result)
{
    switch (result)
    {
        case CLOUD_ATTEMPT_WAIT_FOR_NETWORK:
            return CLOUD_MANAGER_FAILURE_WAIT_FOR_NETWORK;

        case CLOUD_ATTEMPT_RETRYABLE_TRANSPORT:
            return CLOUD_MANAGER_FAILURE_RETRYABLE_TRANSPORT;

        case CLOUD_ATTEMPT_RETRYABLE_HTTP:
            return CLOUD_MANAGER_FAILURE_RETRYABLE_HTTP;

        case CLOUD_ATTEMPT_REAUTH_REQUIRED:
            return CLOUD_MANAGER_FAILURE_REAUTH_REQUIRED;

        case CLOUD_ATTEMPT_AUTH_FATAL:
            return CLOUD_MANAGER_FAILURE_AUTH_FATAL;

        case CLOUD_ATTEMPT_NONRETRYABLE_CONFIG:
            return CLOUD_MANAGER_FAILURE_NONRETRYABLE_CONFIG;

        case CLOUD_ATTEMPT_NONRETRYABLE_INTERNAL:
            return CLOUD_MANAGER_FAILURE_NONRETRYABLE_INTERNAL;

        case CLOUD_ATTEMPT_SUCCESS:
        default:
            return CLOUD_MANAGER_FAILURE_NONE;
    }
}

static const char *cloud_manager_failure_class_to_string(
    cloud_manager_failure_class_t failure_class)
{
    switch (failure_class)
    {
        case CLOUD_MANAGER_FAILURE_NONE:
            return "NONE";

        case CLOUD_MANAGER_FAILURE_WAIT_FOR_NETWORK:
            return "WAIT_FOR_NETWORK";

        case CLOUD_MANAGER_FAILURE_RETRYABLE_TRANSPORT:
            return "RETRYABLE_TRANSPORT";

        case CLOUD_MANAGER_FAILURE_RETRYABLE_HTTP:
            return "RETRYABLE_HTTP";

        case CLOUD_MANAGER_FAILURE_REAUTH_REQUIRED:
            return "REAUTH_REQUIRED";

        case CLOUD_MANAGER_FAILURE_AUTH_FATAL:
            return "AUTH_FATAL";

        case CLOUD_MANAGER_FAILURE_NONRETRYABLE_CONFIG:
            return "NONRETRYABLE_CONFIG";

        case CLOUD_MANAGER_FAILURE_NONRETRYABLE_INTERNAL:
            return "NONRETRYABLE_INTERNAL";

        default:
            return "UNKNOWN";
    }
}

static cloud_attempt_result_t
cloud_manager_classify_auth_failure(
    esp_err_t error,
    int *http_status_out)
{
    firebase_auth_status_t auth_status = {0};

    if (http_status_out != NULL)
    {
        *http_status_out = 0;
    }

    const esp_err_t status_result =
        firebase_auth_get_status(&auth_status);

    if (status_result == ESP_OK)
    {
        if (http_status_out != NULL)
        {
            *http_status_out =
                auth_status.last_http_status;
        }

        if (auth_status.state ==
            FIREBASE_AUTH_STATE_CREDENTIAL_ERROR)
        {
            return CLOUD_ATTEMPT_AUTH_FATAL;
        }

        if (auth_status.state ==
            FIREBASE_AUTH_STATE_NETWORK_ERROR)
        {
            const cloud_network_snapshot_t network =
                cloud_manager_get_network_snapshot();

            return network.has_ipv4_address
                ? CLOUD_ATTEMPT_RETRYABLE_TRANSPORT
                : CLOUD_ATTEMPT_WAIT_FOR_NETWORK;
        }
    }

    return cloud_manager_is_configuration_error(error)
        ? CLOUD_ATTEMPT_NONRETRYABLE_CONFIG
        : CLOUD_ATTEMPT_NONRETRYABLE_INTERNAL;
}

static esp_err_t cloud_manager_build_authenticated_url(
    uint32_t *auth_generation_out)
{
    if (auth_generation_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *auth_generation_out = 0U;

    esp_err_t result =
        firebase_auth_get_valid_id_token(
            s_firebase_id_token,
            sizeof(s_firebase_id_token));

    if (result != ESP_OK)
    {
        return result;
    }

    firebase_auth_status_t auth_status = {0};

    result =
        firebase_auth_get_status(
            &auth_status);

    if (result != ESP_OK)
    {
        return result;
    }

    if ((auth_status.token_generation == 0U) ||
        !auth_status.has_id_token)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const int url_length =
        snprintf(
            s_authenticated_url,
            sizeof(s_authenticated_url),
            "%s?auth=%s&print=silent",
            s_config.firebase_latest_url,
            s_firebase_id_token);

    if ((url_length < 0) ||
        (url_length >=
         (int)sizeof(s_authenticated_url)))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    *auth_generation_out =
        auth_status.token_generation;

    return ESP_OK;
}

static void cloud_manager_reset_http_client(
    const char *reason,
    bool clear_active_identity)
{
    const bool had_client =
        (s_http_client != NULL);

    if (s_http_client != NULL)
    {
        const esp_err_t cleanup_result =
            esp_http_client_cleanup(
                s_http_client);

        /*
         * The handle is unusable after cleanup is attempted, even when ESP-IDF
         * reports a cleanup diagnostic. Never accidentally reuse it.
         */
        s_http_client = NULL;

        if (cleanup_result != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "HTTP client cleanup returned %s",
                esp_err_to_name(cleanup_result));
        }
    }

    cloud_manager_zeroize(
        s_http_client_url,
        sizeof(s_http_client_url));
    if (clear_active_identity)
    {
        cloud_manager_zeroize(
            s_authenticated_url,
            sizeof(s_authenticated_url));
        cloud_manager_zeroize(
            s_firebase_id_token,
            sizeof(s_firebase_id_token));
        cloud_manager_zeroize(
            s_telemetry_payload,
            sizeof(s_telemetry_payload));
    }

    s_http_client_network_epoch = 0U;
    s_http_client_auth_generation = 0U;

    if (had_client)
    {
        cloud_manager_record_http_client_reset();

        ESP_LOGD(
            TAG,
            "HTTP client reset: %s",
            (reason != NULL)
                ? reason
                : "unspecified");
    }
}

static esp_err_t cloud_manager_prepare_http_client(
    uint32_t network_epoch,
    uint32_t auth_generation)
{
    if ((network_epoch == 0U) ||
        (auth_generation == 0U) ||
        (s_authenticated_url[0] == '\0'))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((s_http_client != NULL) &&
        ((s_http_client_network_epoch !=
          network_epoch) ||
         (s_http_client_auth_generation !=
          auth_generation)))
    {
        cloud_manager_reset_http_client(
            (s_http_client_network_epoch !=
             network_epoch)
                ? "network epoch changed"
                : "authentication generation changed",
            false);
    }

    if (s_http_client != NULL)
    {
        return ESP_OK;
    }

    esp_http_client_config_t http_config =
    {
        .url = s_authenticated_url,
        .method = HTTP_METHOD_PUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CLOUD_MANAGER_HTTP_TIMEOUT_MS,
        /* The request line contains the Firebase ID token query string. */
        .buffer_size_tx = CLOUD_MANAGER_HTTP_TX_BUFFER_SIZE,
        .keep_alive_enable = true,
    };

    s_http_client =
        esp_http_client_init(&http_config);

    if (s_http_client == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize HTTP client");

        return ESP_ERR_NO_MEM;
    }

    esp_err_t result =
        esp_http_client_set_header(
            s_http_client,
            "Content-Type",
            "application/json");

    if (result != ESP_OK)
    {
        cloud_manager_reset_http_client(
            "set-header failure",
            true);
        return result;
    }

    memcpy(
        s_http_client_url,
        s_authenticated_url,
        strlen(s_authenticated_url) + 1U);

    s_http_client_network_epoch = network_epoch;
    s_http_client_auth_generation =
        auth_generation;

    return ESP_OK;
}

static void cloud_manager_log_transport_failure(
    esp_err_t error)
{
    if (s_http_client == NULL)
    {
        return;
    }

    int tls_error = 0;
    int tls_flags = 0;
    const int socket_errno =
        esp_http_client_get_errno(s_http_client);

    (void)esp_http_client_get_and_clear_last_tls_error(
        s_http_client,
        &tls_error,
        &tls_flags);

    ESP_LOGW(
        TAG,
        "HTTP transport error=%s, socket_errno=%d, "
        "tls_error=0x%x, tls_flags=0x%x",
        esp_err_to_name(error),
        socket_errno,
        (unsigned int)tls_error,
        (unsigned int)tls_flags);
}

static cloud_attempt_outcome_t
cloud_manager_publish_telemetry(
    const cloud_sensor_telemetry_t *telemetry)
{
    cloud_attempt_outcome_t outcome =
    {
        .result =
            CLOUD_ATTEMPT_NONRETRYABLE_INTERNAL,
        .error = ESP_FAIL,
        .http_status = 0,
        .auth_generation = 0U,
    };

    if (telemetry == NULL)
    {
        outcome.result =
            CLOUD_ATTEMPT_NONRETRYABLE_CONFIG;
        outcome.error = ESP_ERR_INVALID_ARG;
        return outcome;
    }

    const cloud_network_snapshot_t initial_network =
        cloud_manager_get_network_snapshot();

    if (!initial_network.has_ipv4_address)
    {
        outcome.result =
            CLOUD_ATTEMPT_WAIT_FOR_NETWORK;
        outcome.error = ESP_ERR_INVALID_STATE;
        return outcome;
    }

    esp_err_t result =
        cloud_manager_build_authenticated_url(
            &outcome.auth_generation);

    if (result != ESP_OK)
    {
        outcome.result =
            cloud_manager_classify_auth_failure(
                result,
                &outcome.http_status);
        outcome.error = result;

        ESP_LOGW(
            TAG,
            "Firebase Authentication failed: %s",
            esp_err_to_name(result));

        return outcome;
    }

    size_t payload_length = 0U;
    result = cloud_telemetry_json_serialize(
        telemetry,
        s_telemetry_payload,
        sizeof(s_telemetry_payload),
        &payload_length);

    if (result != ESP_OK) {
        outcome.result =
            CLOUD_ATTEMPT_NONRETRYABLE_CONFIG;
        outcome.error = result;
        return outcome;
    }

    result =
        cloud_manager_prepare_http_client(
            initial_network.epoch,
            outcome.auth_generation);

    if (result != ESP_OK)
    {
        outcome.result =
            cloud_manager_is_configuration_error(result)
                ? CLOUD_ATTEMPT_NONRETRYABLE_CONFIG
                : CLOUD_ATTEMPT_NONRETRYABLE_INTERNAL;
        outcome.error = result;
        return outcome;
    }

    result =
        esp_http_client_set_post_field(
            s_http_client,
            s_telemetry_payload,
            (int)payload_length);

    if (result != ESP_OK)
    {
        outcome.result =
            cloud_manager_is_configuration_error(result)
                ? CLOUD_ATTEMPT_NONRETRYABLE_CONFIG
                : CLOUD_ATTEMPT_NONRETRYABLE_INTERNAL;
        outcome.error = result;
        return outcome;
    }

    result =
        esp_http_client_perform(
            s_http_client);

    int response_status = 0;

    if (s_http_client != NULL)
    {
        response_status =
            esp_http_client_get_status_code(
                s_http_client);
    }

    /*
     * Firebase may return an authentication challenge that ESP HTTP Client
     * cannot process as Basic/Digest auth. Preserve the semantic HTTP status.
     */
    if ((result == ESP_ERR_NOT_SUPPORTED) &&
        ((response_status == 401) ||
         (response_status == 403)))
    {
        outcome.http_status = response_status;
    }
    else if (result == ESP_OK)
    {
        outcome.http_status = response_status;
    }

    if ((result == ESP_OK) &&
        (outcome.http_status >= 200) &&
        (outcome.http_status < 300))
    {
        outcome.result = CLOUD_ATTEMPT_SUCCESS;
        outcome.error = ESP_OK;
        return outcome;
    }

    if (outcome.http_status == 401 ||
        outcome.http_status == 403)
    {
        outcome.result =
            CLOUD_ATTEMPT_REAUTH_REQUIRED;
        outcome.error = ESP_FAIL;
        return outcome;
    }

    if (cloud_manager_is_retryable_http_error(
            outcome.http_status))
    {
        outcome.result =
            CLOUD_ATTEMPT_RETRYABLE_HTTP;
        outcome.error = ESP_FAIL;
        return outcome;
    }

    if (result != ESP_OK)
    {
        cloud_manager_log_transport_failure(
            result);

        const cloud_network_snapshot_t network =
            cloud_manager_get_network_snapshot();

        outcome.result =
            network.has_ipv4_address
                ? CLOUD_ATTEMPT_RETRYABLE_TRANSPORT
                : CLOUD_ATTEMPT_WAIT_FOR_NETWORK;
        outcome.error = result;
        return outcome;
    }

    if (outcome.http_status == 0)
    {
        outcome.result =
            CLOUD_ATTEMPT_RETRYABLE_TRANSPORT;
        outcome.error = ESP_FAIL;
        return outcome;
    }

    outcome.result =
        CLOUD_ATTEMPT_NONRETRYABLE_CONFIG;
    outcome.error = ESP_FAIL;

    return outcome;
}

static uint32_t cloud_manager_next_retry_delay(
    uint32_t current_delay_ms)
{
    if (current_delay_ms >=
        CLOUD_MANAGER_RETRY_MAX_MS)
    {
        return CLOUD_MANAGER_RETRY_MAX_MS;
    }

    if (current_delay_ms >
        (CLOUD_MANAGER_RETRY_MAX_MS / 2U))
    {
        return CLOUD_MANAGER_RETRY_MAX_MS;
    }

    const uint32_t doubled_delay_ms =
        current_delay_ms * 2U;

    return
        (doubled_delay_ms >
         CLOUD_MANAGER_RETRY_MAX_MS)
            ? CLOUD_MANAGER_RETRY_MAX_MS
            : doubled_delay_ms;
}

static void cloud_manager_task(
    void *argument)
{
    (void)argument;

    cloud_sensor_telemetry_t pending_telemetry = {0};
    bool has_pending_telemetry = false;
    bool upload_has_succeeded = false;
    bool terminal_error_latched = false;
    bool forced_403_recovery_active = false;

    uint32_t retry_delay_ms =
        CLOUD_MANAGER_RETRY_INITIAL_MS;
    uint32_t observed_network_epoch = 0U;

    bool delay_active = false;
    TickType_t delay_start = 0U;
    TickType_t delay_duration = 0U;

    ESP_LOGD(
        TAG,
        "Cloud task started, publish period=%lu ms",
        (unsigned long)s_config.publish_period_ms);

    while (true)
    {
        cloud_sensor_telemetry_t received_telemetry;

        while (xQueueReceive(
                   s_telemetry_queue,
                   &received_telemetry,
                   0U) == pdTRUE)
        {
            pending_telemetry =
                received_telemetry;
            has_pending_telemetry = true;
        }

        const cloud_network_snapshot_t network =
            cloud_manager_get_network_snapshot();

        if (network.epoch !=
            observed_network_epoch)
        {
            const uint32_t previous_epoch =
                observed_network_epoch;

            observed_network_epoch =
                network.epoch;

            cloud_manager_record_network_epoch(
                network.epoch);

            cloud_manager_reset_http_client(
                "network epoch changed",
                true);

            ESP_LOGD(
                TAG,
                "Network epoch changed: %lu -> %lu, online=%s",
                (unsigned long)previous_epoch,
                (unsigned long)network.epoch,
                network.has_ipv4_address
                    ? "true"
                    : "false");

            if (network.has_ipv4_address &&
                !terminal_error_latched)
            {
                retry_delay_ms =
                    CLOUD_MANAGER_RETRY_INITIAL_MS;
                delay_active = false;
            }
        }

        if (!network.has_ipv4_address)
        {
            cloud_manager_reset_http_client(
                "network offline",
                true);

            if (!terminal_error_latched)
            {
                cloud_manager_set_state(
                    CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK);
            }

            cloud_manager_wait_for_notification(
                portMAX_DELAY);
            continue;
        }

        if (terminal_error_latched)
        {
            cloud_manager_wait_for_notification(
                portMAX_DELAY);
            continue;
        }

        if (!has_pending_telemetry)
        {
            if (!upload_has_succeeded)
            {
                cloud_manager_set_state(
                    CLOUD_MANAGER_STATE_WAITING_FOR_DATA);
            }

            cloud_manager_wait_for_notification(
                portMAX_DELAY);
            continue;
        }

        if (delay_active)
        {
            const TickType_t remaining_ticks =
                cloud_manager_delay_remaining(
                    delay_start,
                    delay_duration);

            if (remaining_ticks > 0U)
            {
                cloud_manager_wait_for_notification(
                    remaining_ticks);
                continue;
            }

            delay_active = false;
        }

        cloud_manager_set_state(
            CLOUD_MANAGER_STATE_UPLOADING);
        cloud_manager_record_attempt_start();

        const cloud_attempt_outcome_t outcome =
            cloud_manager_publish_telemetry(
                &pending_telemetry);

        if (outcome.result ==
            CLOUD_ATTEMPT_SUCCESS)
        {
            cloud_manager_record_upload_success(
                outcome.http_status);

            ESP_LOGD(
                TAG,
                "Cloud recovered to ONLINE");

            /*
             * Only the snapshot copied into this attempt is cleared. A newer
             * length-one queue value posted while HTTPS was active remains
             * queued and will become pending on the next loop.
             */
            has_pending_telemetry = false;
            upload_has_succeeded = true;
            forced_403_recovery_active = false;
            retry_delay_ms =
                CLOUD_MANAGER_RETRY_INITIAL_MS;

            delay_start =
                xTaskGetTickCount();
            delay_duration =
                cloud_manager_ms_to_ticks_nonzero(
                    s_config.publish_period_ms);
            delay_active = true;

            continue;
        }

        const cloud_manager_failure_class_t
            failure_class =
                cloud_manager_failure_class_from_attempt(
                    outcome.result);

        ESP_LOGD(
            TAG,
            "Cloud attempt classified as %s",
            cloud_manager_failure_class_to_string(
                failure_class));

        if (outcome.result ==
            CLOUD_ATTEMPT_WAIT_FOR_NETWORK)
        {
            cloud_manager_reset_http_client(
                "network lost during attempt",
                true);
            cloud_manager_record_upload_failure(
                &outcome,
                CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK,
                CLOUD_MANAGER_RETRY_INITIAL_MS);
            delay_active = false;
            continue;
        }

        if (outcome.result ==
            CLOUD_ATTEMPT_REAUTH_REQUIRED)
        {
            cloud_manager_reset_http_client(
                (outcome.http_status == 403)
                    ? "HTTP 403"
                    : "HTTP 401",
                true);

            if ((outcome.http_status == 403) &&
                forced_403_recovery_active)
            {
                cloud_attempt_outcome_t fatal_outcome =
                    outcome;

                fatal_outcome.result =
                    CLOUD_ATTEMPT_AUTH_FATAL;

                cloud_manager_record_upload_failure(
                    &fatal_outcome,
                    CLOUD_MANAGER_STATE_AUTH_ERROR,
                    retry_delay_ms);
                terminal_error_latched = true;
                continue;
            }

            const esp_err_t invalidate_result =
                firebase_auth_invalidate_id_token();

            if (invalidate_result != ESP_OK)
            {
                cloud_attempt_outcome_t fatal_outcome =
                    outcome;

                fatal_outcome.result =
                    CLOUD_ATTEMPT_AUTH_FATAL;
                fatal_outcome.error =
                    invalidate_result;

                cloud_manager_record_upload_failure(
                    &fatal_outcome,
                    CLOUD_MANAGER_STATE_AUTH_ERROR,
                    retry_delay_ms);
                terminal_error_latched = true;
                continue;
            }

            cloud_manager_record_auth_recovery();

            if (outcome.http_status == 403)
            {
                forced_403_recovery_active = true;
            }

            cloud_manager_record_upload_failure(
                &outcome,
                CLOUD_MANAGER_STATE_RETRY_WAIT,
                retry_delay_ms);

            ESP_LOGD(
                TAG,
                "Authentication recovery scheduled in %lu ms",
                (unsigned long)retry_delay_ms);

            delay_start =
                xTaskGetTickCount();
            delay_duration =
                cloud_manager_ms_to_ticks_nonzero(
                    retry_delay_ms);
            delay_active = true;
            retry_delay_ms =
                cloud_manager_next_retry_delay(
                    retry_delay_ms);
            continue;
        }

        if ((outcome.result ==
             CLOUD_ATTEMPT_RETRYABLE_TRANSPORT) ||
            (outcome.result ==
             CLOUD_ATTEMPT_RETRYABLE_HTTP))
        {
            cloud_manager_reset_http_client(
                (outcome.result ==
                 CLOUD_ATTEMPT_RETRYABLE_HTTP)
                    ? "retryable HTTP response"
                    : "transport failure",
                true);

            cloud_manager_record_upload_failure(
                &outcome,
                CLOUD_MANAGER_STATE_RETRY_WAIT,
                retry_delay_ms);

            ESP_LOGD(
                TAG,
                "Retry class=%s, delay=%lu ms",
                cloud_manager_failure_class_to_string(
                    failure_class),
                (unsigned long)retry_delay_ms);

            delay_start =
                xTaskGetTickCount();
            delay_duration =
                cloud_manager_ms_to_ticks_nonzero(
                    retry_delay_ms);
            delay_active = true;
            retry_delay_ms =
                cloud_manager_next_retry_delay(
                    retry_delay_ms);
            continue;
        }

        cloud_manager_reset_http_client(
            (outcome.result ==
             CLOUD_ATTEMPT_AUTH_FATAL)
                ? "fatal authentication failure"
                : "non-retryable failure",
            true);

        cloud_manager_record_upload_failure(
            &outcome,
            (outcome.result ==
             CLOUD_ATTEMPT_AUTH_FATAL)
                ? CLOUD_MANAGER_STATE_AUTH_ERROR
                : CLOUD_MANAGER_STATE_ERROR,
            retry_delay_ms);

        terminal_error_latched = true;
    }
}

/* Functions ---------------------------------------------------------------- */
esp_err_t cloud_manager_init(
    const cloud_manager_config_t *config)
{
    if ((config == NULL) ||
        (config->firebase_latest_url == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (config->publish_period_ms <
        CLOUD_MANAGER_MIN_PUBLISH_PERIOD_MS)
    {
        ESP_LOGE(
            TAG,
            "Publish period must be at least %u ms",
            CLOUD_MANAGER_MIN_PUBLISH_PERIOD_MS);

        return ESP_ERR_INVALID_ARG;
    }

    const size_t url_length =
        strlen(config->firebase_latest_url);

    if ((url_length == 0U) ||
        (url_length >=
         sizeof(s_firebase_latest_url)) ||
        (strchr(
             config->firebase_latest_url,
             '?') != NULL))
    {
        ESP_LOGE(
            TAG,
            "Firebase URL must be a base .json URL without query parameters");

        return ESP_ERR_INVALID_ARG;
    }

    memcpy(
        s_firebase_latest_url,
        config->firebase_latest_url,
        url_length + 1U);

    s_config.firebase_latest_url =
        s_firebase_latest_url;
    s_config.publish_period_ms =
        config->publish_period_ms;

    s_status_mutex =
        xSemaphoreCreateMutex();

    if (s_status_mutex == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to create status mutex");

        return ESP_ERR_NO_MEM;
    }

    s_telemetry_queue =
        xQueueCreateWithCaps(
            CLOUD_MANAGER_TELEMETRY_QUEUE_LENGTH,
            sizeof(cloud_sensor_telemetry_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (s_telemetry_queue == NULL)
    {
        vSemaphoreDelete(s_status_mutex);
        s_status_mutex = NULL;

        ESP_LOGE(
            TAG,
            "Failed to create telemetry queue");

        return ESP_ERR_NO_MEM;
    }

    memset(
        &s_status,
        0,
        sizeof(s_status));

    s_status.state =
        CLOUD_MANAGER_STATE_INITIALIZED;
    s_status.last_error = ESP_OK;
    s_status.current_retry_delay_ms =
        CLOUD_MANAGER_RETRY_INITIAL_MS;
    s_status.last_failure_class =
        CLOUD_MANAGER_FAILURE_NONE;

    s_status_callback = NULL;
    s_status_callback_user_data = NULL;

    s_http_client = NULL;
    s_http_client_network_epoch = 0U;
    s_http_client_auth_generation = 0U;
    s_cloud_task_handle = NULL;

    cloud_manager_zeroize(
        s_http_client_url,
        sizeof(s_http_client_url));
    cloud_manager_zeroize(
        s_authenticated_url,
        sizeof(s_authenticated_url));
    cloud_manager_zeroize(
        s_firebase_id_token,
        sizeof(s_firebase_id_token));

    portENTER_CRITICAL(&s_signal_lock);
    s_network_has_ipv4_address = false;
    s_network_epoch = 1U;
    portEXIT_CRITICAL(&s_signal_lock);

    s_status.network_epoch = 1U;
    s_is_initialized = true;

    ESP_LOGI(
        TAG,
        "Initialized");

    return ESP_OK;
}

esp_err_t cloud_manager_start(void)
{
    if (!s_is_initialized ||
        s_is_started)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_is_started = true;

    TaskHandle_t task_handle = NULL;

    const BaseType_t task_result =
        xTaskCreateWithCaps(
            cloud_manager_task,
            CLOUD_MANAGER_TASK_NAME,
            CLOUD_MANAGER_TASK_STACK_SIZE,
            NULL,
            CLOUD_MANAGER_TASK_PRIORITY,
            &task_handle,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (task_result != pdPASS)
    {
        s_is_started = false;

        ESP_LOGE(
            TAG,
            "Failed to create cloud task");

        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_signal_lock);
    s_cloud_task_handle = task_handle;
    portEXIT_CRITICAL(&s_signal_lock);

    /*
     * Close the creation race: state/queue changes that happened while the
     * handle was not yet published are already retained, and this wake makes
     * the new task inspect both immediately.
     */
    cloud_manager_wake_task(
        CLOUD_MANAGER_NOTIFY_ALL);

    ESP_LOGI(
        TAG,
        "Started");

    return ESP_OK;
}

esp_err_t cloud_manager_register_status_callback(
    cloud_manager_status_callback_t callback,
    void *user_data)
{
    if (!s_is_initialized ||
        s_status_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_status_mutex,
            pdMS_TO_TICKS(
                CLOUD_MANAGER_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_status_callback = callback;
    s_status_callback_user_data = user_data;

    xSemaphoreGive(s_status_mutex);

    return ESP_OK;
}

esp_err_t cloud_manager_notify_network_state(
    bool has_ipv4_address)
{
    if (!s_is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    bool state_changed = false;
    TaskHandle_t task_handle = NULL;

    portENTER_CRITICAL(&s_signal_lock);

    if (s_network_has_ipv4_address !=
        has_ipv4_address)
    {
        s_network_has_ipv4_address =
            has_ipv4_address;
        s_network_epoch++;

        if (s_network_epoch == 0U)
        {
            s_network_epoch = 1U;
        }

        state_changed = true;
    }

    task_handle = s_cloud_task_handle;

    portEXIT_CRITICAL(&s_signal_lock);

    if (state_changed &&
        (task_handle != NULL))
    {
        (void)xTaskNotify(
            task_handle,
            CLOUD_MANAGER_NOTIFY_NETWORK_CHANGED,
            eSetBits);
    }

    return ESP_OK;
}

esp_err_t cloud_manager_post_sensor_telemetry(
    const cloud_sensor_telemetry_t *telemetry)
{
    if (telemetry == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_is_initialized ||
        s_telemetry_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (telemetry->data_valid &&
        (!isfinite(telemetry->temperature_c) ||
         !isfinite(telemetry->humidity_percent)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((telemetry->audio.state <
         CLOUD_AUDIO_STATE_UNAVAILABLE) ||
        (telemetry->audio.state >
         CLOUD_AUDIO_STATE_ERROR))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!cloud_telemetry_json_is_valid_time_telemetry(
            &telemetry->time))
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* Queue length is one: always retain the latest telemetry snapshot. */
    if (xQueueOverwrite(
            s_telemetry_queue,
            telemetry) != pdPASS)
    {
        return ESP_FAIL;
    }

    cloud_manager_wake_task(
        CLOUD_MANAGER_NOTIFY_TELEMETRY_AVAILABLE);

    return ESP_OK;
}

esp_err_t cloud_manager_get_status(
    cloud_manager_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_is_initialized ||
        s_status_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_status_mutex,
            pdMS_TO_TICKS(
                CLOUD_MANAGER_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    *status = s_status;

    xSemaphoreGive(s_status_mutex);

    return ESP_OK;
}
