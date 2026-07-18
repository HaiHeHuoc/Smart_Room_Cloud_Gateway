/* Includes ----------------------------------------------------------------- */
#include "cloud_manager.h"

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

#include "firebase_auth.h"
#include "wifi_manager.h"

/* Macros ------------------------------------------------------------------- */
#define CLOUD_MANAGER_TASK_NAME                    "cloud_manager"
#define CLOUD_MANAGER_TASK_STACK_SIZE              12288U
#define CLOUD_MANAGER_TASK_PRIORITY                4U

#define CLOUD_MANAGER_TELEMETRY_QUEUE_LENGTH       1U

#define CLOUD_MANAGER_HTTP_TIMEOUT_MS              15000U
#define CLOUD_MANAGER_JSON_BUFFER_SIZE             320U
#define CLOUD_MANAGER_URL_BUFFER_SIZE              256U
#define CLOUD_MANAGER_AUTH_URL_BUFFER_SIZE          \
    (CLOUD_MANAGER_URL_BUFFER_SIZE +                \
     FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE + 64U)
#define CLOUD_MANAGER_HTTP_TX_BUFFER_SIZE            \
    (CLOUD_MANAGER_AUTH_URL_BUFFER_SIZE + 256U)

#define CLOUD_MANAGER_MIN_PUBLISH_PERIOD_MS        1000U

#define CLOUD_MANAGER_RETRY_INITIAL_MS             5000U
#define CLOUD_MANAGER_RETRY_MAX_MS                 60000U
#define CLOUD_MANAGER_NETWORK_CHECK_MS             1000U
#define CLOUD_MANAGER_MUTEX_TIMEOUT_MS             100U

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "CLOUD_MANAGER";

/* Static Variables --------------------------------------------------------- */
static cloud_manager_config_t s_config;

static char s_firebase_latest_url[
    CLOUD_MANAGER_URL_BUFFER_SIZE];

static char s_firebase_id_token[
    FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE];

static char s_authenticated_url[
    CLOUD_MANAGER_AUTH_URL_BUFFER_SIZE];

static QueueHandle_t s_telemetry_queue;
static TaskHandle_t s_cloud_task_handle;

static bool s_is_initialized;
static bool s_is_started;

static cloud_manager_status_t s_status;
static SemaphoreHandle_t s_status_mutex;

/* Function Prototypes ------------------------------------------------------ */
static int64_t cloud_manager_get_time_ms(void);

static void cloud_manager_set_state(
    cloud_manager_state_t new_state);

static void cloud_manager_record_upload_success(
    int http_status);

static void cloud_manager_record_upload_failure(
    esp_err_t error,
    int http_status,
    uint32_t retry_delay_ms);

static bool cloud_manager_is_auth_error(
    int http_status);

static bool cloud_manager_is_retryable_http_error(
    int http_status);

static esp_err_t cloud_manager_build_authenticated_url(void);

static esp_err_t cloud_manager_publish_telemetry(
    const cloud_sensor_telemetry_t *telemetry,
    int *http_status_out);

static void cloud_manager_task(
    void *argument);

/* Static Functions --------------------------------------------------------- */
static int64_t cloud_manager_get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
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
        ESP_LOGI(
            TAG,
            "Cloud state changed: %d -> %d",
            old_state,
            new_state);
    }
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

    s_status.successful_upload_count++;
    s_status.consecutive_failure_count = 0U;

    s_status.current_retry_delay_ms =
        CLOUD_MANAGER_RETRY_INITIAL_MS;

    s_status.last_success_time_ms =
        cloud_manager_get_time_ms();

    xSemaphoreGive(s_status_mutex);
}

static bool cloud_manager_is_auth_error(
    int http_status)
{
    return http_status == 401 ||
           http_status == 403;
}

static bool cloud_manager_is_retryable_http_error(
    int http_status)
{
    return http_status == 408 ||
           http_status == 429 ||
           http_status >= 500;
}

static void cloud_manager_record_upload_failure(
    esp_err_t error,
    int http_status,
    uint32_t retry_delay_ms)
{
    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.last_error = error;
    s_status.last_http_status = http_status;

    s_status.failed_upload_count++;
    s_status.consecutive_failure_count++;

    s_status.current_retry_delay_ms =
        retry_delay_ms;

    if (cloud_manager_is_auth_error(http_status))
    {
        s_status.state =
            CLOUD_MANAGER_STATE_AUTH_ERROR;
    }
    else if (http_status == 0 ||
             cloud_manager_is_retryable_http_error(
                 http_status))
    {
        s_status.state =
            CLOUD_MANAGER_STATE_RETRY_WAIT;
    }
    else
    {
        s_status.state =
            CLOUD_MANAGER_STATE_ERROR;
    }

    xSemaphoreGive(s_status_mutex);
}

static esp_err_t cloud_manager_build_authenticated_url(void)
{
    esp_err_t auth_result =
        firebase_auth_get_valid_id_token(
            s_firebase_id_token,
            sizeof(s_firebase_id_token));

    if (auth_result != ESP_OK)
    {
        return auth_result;
    }

    int url_length =
        snprintf(
            s_authenticated_url,
            sizeof(s_authenticated_url),
            "%s?auth=%s&print=silent",
            s_config.firebase_latest_url,
            s_firebase_id_token);

    if (url_length < 0 ||
        url_length >= (int)sizeof(s_authenticated_url))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t cloud_manager_publish_telemetry(
    const cloud_sensor_telemetry_t *telemetry,
    int *http_status_out)
{
    if (telemetry == NULL ||
        http_status_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *http_status_out = 0;

    esp_err_t result =
        cloud_manager_build_authenticated_url();

    if (result != ESP_OK)
    {
        firebase_auth_status_t auth_status;

        if (firebase_auth_get_status(
                &auth_status) == ESP_OK &&
            auth_status.state ==
                FIREBASE_AUTH_STATE_CREDENTIAL_ERROR)
        {
            *http_status_out = 401;
        }

        ESP_LOGW(
            TAG,
            "Firebase Authentication failed: %s",
            esp_err_to_name(result));

        return result;
    }

    char payload[CLOUD_MANAGER_JSON_BUFFER_SIZE];

    int payload_length =
        snprintf(
            payload,
            sizeof(payload),
            "{"
                "\"temperature_c\":%.1f,"
                "\"humidity_percent\":%.1f,"
                "\"sensor_valid\":%s,"
                "\"sensor_stale\":%s,"
                "\"sensor_state\":%ld,"
                "\"last_error\":%ld,"
                "\"sample_uptime_ms\":%lld,"
                "\"source\":\"esp32_cloud_manager\""
            "}",
            telemetry->temperature_c,
            telemetry->humidity_percent,
            telemetry->data_valid ? "true" : "false",
            telemetry->data_stale ? "true" : "false",
            (long)telemetry->sensor_state,
            (long)telemetry->last_error,
            (long long)telemetry->sample_uptime_ms);

    if (payload_length < 0 ||
        payload_length >= (int)sizeof(payload))
    {
        ESP_LOGE(
            TAG,
            "Telemetry JSON buffer is too small");

        return ESP_ERR_INVALID_SIZE;
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

    esp_http_client_handle_t client =
        esp_http_client_init(&http_config);

    if (client == NULL)
    {
        ESP_LOGE(
            TAG,
            "Failed to initialize HTTP client");

        return ESP_ERR_NO_MEM;
    }

    result =
        esp_http_client_set_header(
            client,
            "Content-Type",
            "application/json");

    if (result == ESP_OK)
    {
        result =
            esp_http_client_set_post_field(
                client,
                payload,
                payload_length);
    }

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Publishing telemetry: T=%.1f C, H=%.1f %%",
            telemetry->temperature_c,
            telemetry->humidity_percent);

        result =
            esp_http_client_perform(client);
    }

    int http_status =
        esp_http_client_get_status_code(client);

    /*
     * Firebase may return an authentication challenge that ESP HTTP Client
     * cannot process as Basic/Digest auth. Preserve the semantic auth error.
     */
    if (result == ESP_ERR_NOT_SUPPORTED &&
        http_status == 0)
    {
        http_status = 401;
    }

    *http_status_out = http_status;

    if (http_status == 401)
    {
        firebase_auth_invalidate_id_token();
    }

    esp_http_client_cleanup(client);

    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Firebase request failed: %s, HTTP=%d",
            esp_err_to_name(result),
            http_status);

        return result;
    }

    ESP_LOGI(
        TAG,
        "Firebase HTTP status: %d",
        http_status);

    if (http_status < 200 ||
        http_status >= 300)
    {
        ESP_LOGW(
            TAG,
            "Firebase rejected telemetry: HTTP %d",
            http_status);

        return ESP_FAIL;
    }

    return ESP_OK;
}

static void cloud_manager_task(
    void *argument)
{
    (void)argument;

    cloud_sensor_telemetry_t pending_telemetry = {0};
    bool has_pending_telemetry = false;

    uint32_t retry_delay_ms =
        CLOUD_MANAGER_RETRY_INITIAL_MS;

    uint32_t next_delay_ms =
        s_config.publish_period_ms;

    ESP_LOGI(
        TAG,
        "Cloud task started, publish period=%lu ms",
        (unsigned long)s_config.publish_period_ms);

    while (true)
    {
        cloud_sensor_telemetry_t received_telemetry;

        while (xQueueReceive(
                   s_telemetry_queue,
                   &received_telemetry,
                   0) == pdTRUE)
        {
            pending_telemetry =
                received_telemetry;

            has_pending_telemetry = true;
        }

        if (!has_pending_telemetry)
        {
            if (wifi_manager_is_connected())
            {
                cloud_manager_set_state(
                    CLOUD_MANAGER_STATE_WAITING_FOR_DATA);
            }
            else
            {
                cloud_manager_set_state(
                    CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK);
            }

            next_delay_ms =
                CLOUD_MANAGER_NETWORK_CHECK_MS;
        }
        else if (!wifi_manager_is_connected())
        {
            cloud_manager_set_state(
                CLOUD_MANAGER_STATE_WAITING_FOR_NETWORK);

            next_delay_ms =
                CLOUD_MANAGER_NETWORK_CHECK_MS;
        }
        else
        {
            cloud_manager_set_state(
                CLOUD_MANAGER_STATE_UPLOADING);

            int http_status = 0;

            esp_err_t error =
                cloud_manager_publish_telemetry(
                    &pending_telemetry,
                    &http_status);

            if (error == ESP_OK)
            {
                cloud_manager_record_upload_success(
                    http_status);

                ESP_LOGI(
                    TAG,
                    "Telemetry published successfully");

                has_pending_telemetry = false;

                retry_delay_ms =
                    CLOUD_MANAGER_RETRY_INITIAL_MS;

                next_delay_ms =
                    s_config.publish_period_ms;
            }
            else
            {
                cloud_manager_record_upload_failure(
                    error,
                    http_status,
                    retry_delay_ms);

                ESP_LOGW(
                    TAG,
                    "Upload failed: error=%s, HTTP=%d, "
                    "retry in %lu ms",
                    esp_err_to_name(error),
                    http_status,
                    (unsigned long)retry_delay_ms);

                next_delay_ms =
                    retry_delay_ms;

                if (retry_delay_ms <
                    CLOUD_MANAGER_RETRY_MAX_MS)
                {
                    uint32_t doubled_delay_ms =
                        retry_delay_ms * 2U;

                    retry_delay_ms =
                        doubled_delay_ms >
                        CLOUD_MANAGER_RETRY_MAX_MS
                            ? CLOUD_MANAGER_RETRY_MAX_MS
                            : doubled_delay_ms;
                }
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(next_delay_ms));
    }
}

/* Functions ---------------------------------------------------------------- */
esp_err_t cloud_manager_init(
    const cloud_manager_config_t *config)
{
    if (config == NULL ||
        config->firebase_latest_url == NULL)
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

    size_t url_length =
        strlen(config->firebase_latest_url);

    if (url_length == 0U ||
        url_length >= sizeof(s_firebase_latest_url) ||
        strchr(config->firebase_latest_url, '?') != NULL)
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
        xQueueCreate(
            CLOUD_MANAGER_TELEMETRY_QUEUE_LENGTH,
            sizeof(cloud_sensor_telemetry_t));

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

    BaseType_t task_result =
        xTaskCreate(
            cloud_manager_task,
            CLOUD_MANAGER_TASK_NAME,
            CLOUD_MANAGER_TASK_STACK_SIZE,
            NULL,
            CLOUD_MANAGER_TASK_PRIORITY,
            &s_cloud_task_handle);

    if (task_result != pdPASS)
    {
        s_is_started = false;
        s_cloud_task_handle = NULL;

        ESP_LOGE(
            TAG,
            "Failed to create cloud task");

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Started");

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

    /* Queue length is one: always retain the latest telemetry snapshot. */
    if (xQueueOverwrite(
            s_telemetry_queue,
            telemetry) != pdPASS)
    {
        return ESP_FAIL;
    }

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
