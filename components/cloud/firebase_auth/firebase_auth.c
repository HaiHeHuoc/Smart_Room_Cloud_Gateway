/* Includes ----------------------------------------------------------------- */
#include "firebase_auth.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Macros ------------------------------------------------------------------- */
#define FIREBASE_AUTH_API_KEY_BUFFER_SIZE       192U
#define FIREBASE_AUTH_EMAIL_BUFFER_SIZE         192U
#define FIREBASE_AUTH_PASSWORD_BUFFER_SIZE      192U
#define FIREBASE_AUTH_UID_BUFFER_SIZE           192U
#define FIREBASE_AUTH_REFRESH_TOKEN_BUFFER_SIZE 2048U

#define FIREBASE_AUTH_RESPONSE_BUFFER_SIZE      8192U
#define FIREBASE_AUTH_URL_BUFFER_SIZE           512U
#define FIREBASE_AUTH_REQUEST_BUFFER_SIZE       4096U

#define FIREBASE_AUTH_HTTP_TIMEOUT_MS           15000U
#define FIREBASE_AUTH_DEFAULT_MARGIN_SECONDS    300U
#define FIREBASE_AUTH_MUTEX_TIMEOUT_MS          100U

#define FIREBASE_SIGN_IN_URL_FORMAT                                      \
    "https://identitytoolkit.googleapis.com/v1/"                         \
    "accounts:signInWithPassword?key=%s"

#define FIREBASE_REFRESH_URL_FORMAT                                      \
    "https://securetoken.googleapis.com/v1/token?key=%s"

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "FIREBASE_AUTH";

/* Type Definitions --------------------------------------------------------- */
typedef struct
{
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
} firebase_auth_response_context_t;

/* Static Variables --------------------------------------------------------- */
static firebase_auth_config_t s_config;
static firebase_auth_status_t s_status;

static char s_api_key[FIREBASE_AUTH_API_KEY_BUFFER_SIZE];
static char s_email[FIREBASE_AUTH_EMAIL_BUFFER_SIZE];
static char s_password[FIREBASE_AUTH_PASSWORD_BUFFER_SIZE];
static char s_expected_uid[FIREBASE_AUTH_UID_BUFFER_SIZE];

static char s_id_token[FIREBASE_AUTH_ID_TOKEN_BUFFER_SIZE];
static char s_refresh_token[FIREBASE_AUTH_REFRESH_TOKEN_BUFFER_SIZE];
static char s_user_uid[FIREBASE_AUTH_UID_BUFFER_SIZE];

/* These buffers are single-owned while s_operation_mutex is held. */
static char s_response_buffer[FIREBASE_AUTH_RESPONSE_BUFFER_SIZE];
static char s_url_buffer[FIREBASE_AUTH_URL_BUFFER_SIZE];
static char s_request_buffer[FIREBASE_AUTH_REQUEST_BUFFER_SIZE];
static char s_refresh_token_snapshot[
    FIREBASE_AUTH_REFRESH_TOKEN_BUFFER_SIZE];

/*
 * The operation mutex serializes the static request buffers and synchronous
 * sign-in/refresh work. The state mutex protects only short token/status
 * snapshots so status reads and invalidation do not wait behind HTTPS.
 */
static SemaphoreHandle_t s_operation_mutex;
static SemaphoreHandle_t s_state_mutex;
static bool s_is_initialized;

/* Function Prototypes ------------------------------------------------------ */
static int64_t firebase_auth_get_time_ms(void);

static void firebase_auth_zeroize(
    void *buffer,
    size_t buffer_size);

static void firebase_auth_increment_generation_locked(void);

static void firebase_auth_record_local_failure(
    esp_err_t error);

static esp_err_t firebase_auth_copy_string(
    char *destination,
    size_t destination_size,
    const char *source);

static esp_err_t firebase_auth_http_event_handler(
    esp_http_client_event_t *event);

static esp_err_t firebase_auth_http_post(
    const char *url,
    const char *content_type,
    const char *body,
    int *http_status_out);

static bool firebase_auth_token_is_valid_locked(void);

static void firebase_auth_log_server_error(void);

static esp_err_t firebase_auth_parse_tokens(
    bool is_refresh_response);

static esp_err_t firebase_auth_sign_in(void);

static esp_err_t firebase_auth_refresh(void);

static esp_err_t firebase_auth_url_encode(
    const char *input,
    char *output,
    size_t output_size);

/* Static Functions --------------------------------------------------------- */
static int64_t firebase_auth_get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void firebase_auth_zeroize(
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

static void firebase_auth_increment_generation_locked(void)
{
    s_status.token_generation++;

    if (s_status.token_generation == 0U)
    {
        s_status.token_generation = 1U;
    }
}

static void firebase_auth_record_local_failure(
    esp_err_t error)
{
    if (xSemaphoreTake(
            s_state_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.state =
        FIREBASE_AUTH_STATE_INTERNAL_ERROR;
    s_status.last_error = error;
    s_status.last_http_status = 0;
    s_status.failed_request_count++;

    xSemaphoreGive(s_state_mutex);
}

static esp_err_t firebase_auth_copy_string(
    char *destination,
    size_t destination_size,
    const char *source)
{
    if (destination == NULL ||
        destination_size == 0U ||
        source == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t source_length = strlen(source);

    if (source_length >= destination_size)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(
        destination,
        source,
        source_length + 1U);

    return ESP_OK;
}

static esp_err_t firebase_auth_http_event_handler(
    esp_http_client_event_t *event)
{
    if (event == NULL ||
        event->user_data == NULL ||
        event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == NULL ||
        event->data_len <= 0)
    {
        return ESP_OK;
    }

    firebase_auth_response_context_t *context =
        (firebase_auth_response_context_t *)event->user_data;

    size_t incoming_size =
        (size_t)event->data_len;

    if (context->length + incoming_size >=
        context->capacity)
    {
        context->overflow = true;
        return ESP_OK;
    }

    memcpy(
        context->buffer + context->length,
        event->data,
        incoming_size);

    context->length += incoming_size;
    context->buffer[context->length] = '\0';

    return ESP_OK;
}

static esp_err_t firebase_auth_http_post(
    const char *url,
    const char *content_type,
    const char *body,
    int *http_status_out)
{
    if (url == NULL ||
        content_type == NULL ||
        body == NULL ||
        http_status_out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        s_response_buffer,
        0,
        sizeof(s_response_buffer));

    *http_status_out = 0;

    firebase_auth_response_context_t response_context =
    {
        .buffer = s_response_buffer,
        .capacity = sizeof(s_response_buffer),
        .length = 0U,
        .overflow = false,
    };

    esp_http_client_config_t http_config =
    {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FIREBASE_AUTH_HTTP_TIMEOUT_MS,
        .event_handler = firebase_auth_http_event_handler,
        .user_data = &response_context,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&http_config);

    if (client == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result =
        esp_http_client_set_header(
            client,
            "Content-Type",
            content_type);

    if (result == ESP_OK)
    {
        result =
            esp_http_client_set_post_field(
                client,
                body,
                strlen(body));
    }

    if (result == ESP_OK)
    {
        result =
            esp_http_client_perform(client);
    }

    *http_status_out =
        esp_http_client_get_status_code(client);

    const esp_err_t cleanup_result =
        esp_http_client_cleanup(client);

    /*
     * The one-shot handle is unusable after cleanup is attempted. Preserve
     * the authentication result, but expose a cleanup diagnostic instead of
     * silently implying the handle could be reused.
     */
    if (cleanup_result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Firebase Auth HTTP cleanup returned %s",
            esp_err_to_name(cleanup_result));
    }

    if (response_context.overflow)
    {
        ESP_LOGE(
            TAG,
            "Firebase Auth response buffer overflow");

        firebase_auth_zeroize(
            s_response_buffer,
            sizeof(s_response_buffer));

        return ESP_ERR_INVALID_SIZE;
    }

    return result;
}

static bool firebase_auth_token_is_valid_locked(void)
{
    if (s_id_token[0] == '\0')
    {
        return false;
    }

    int64_t refresh_margin_ms =
        (int64_t)s_config.refresh_margin_seconds *
        1000LL;

    return firebase_auth_get_time_ms() +
           refresh_margin_ms <
           s_status.token_expiry_uptime_ms;
}

static void firebase_auth_log_server_error(void)
{
    if (s_response_buffer[0] == '\0')
    {
        return;
    }

    cJSON *root =
        cJSON_Parse(s_response_buffer);

    if (root == NULL)
    {
        return;
    }

    cJSON *error_object =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "error");

    cJSON *message = NULL;

    if (cJSON_IsObject(error_object))
    {
        message =
            cJSON_GetObjectItemCaseSensitive(
                error_object,
                "message");
    }

    if (cJSON_IsString(message) &&
        message->valuestring != NULL)
    {
        /* Log only the Firebase error name, never credentials or tokens. */
        ESP_LOGW(
            TAG,
            "Firebase Auth server error: %s",
            message->valuestring);
    }

    cJSON_Delete(root);
}

static esp_err_t firebase_auth_parse_tokens(
    bool is_refresh_response)
{
    char current_uid[FIREBASE_AUTH_UID_BUFFER_SIZE] = {0};

    if (is_refresh_response)
    {
        if (xSemaphoreTake(
                s_state_mutex,
                portMAX_DELAY) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        memcpy(
            current_uid,
            s_user_uid,
            sizeof(current_uid));

        xSemaphoreGive(s_state_mutex);
    }

    cJSON *root =
        cJSON_Parse(s_response_buffer);

    if (root == NULL)
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *id_token_field =
        is_refresh_response
            ? "id_token"
            : "idToken";

    const char *refresh_token_field =
        is_refresh_response
            ? "refresh_token"
            : "refreshToken";

    const char *expiry_field =
        is_refresh_response
            ? "expires_in"
            : "expiresIn";

    const char *uid_field =
        is_refresh_response
            ? "user_id"
            : "localId";

    cJSON *id_token =
        cJSON_GetObjectItemCaseSensitive(
            root,
            id_token_field);

    cJSON *refresh_token =
        cJSON_GetObjectItemCaseSensitive(
            root,
            refresh_token_field);

    cJSON *expires_in =
        cJSON_GetObjectItemCaseSensitive(
            root,
            expiry_field);

    cJSON *uid =
        cJSON_GetObjectItemCaseSensitive(
            root,
            uid_field);

    if (!cJSON_IsString(id_token) ||
        id_token->valuestring == NULL ||
        !cJSON_IsString(refresh_token) ||
        refresh_token->valuestring == NULL ||
        !cJSON_IsString(expires_in) ||
        expires_in->valuestring == NULL ||
        (!is_refresh_response &&
         (!cJSON_IsString(uid) ||
          uid->valuestring == NULL)))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *uid_value =
        (cJSON_IsString(uid) &&
         uid->valuestring != NULL)
            ? uid->valuestring
            : current_uid;

    char *expiry_end = NULL;

    unsigned long expires_seconds =
        strtoul(
            expires_in->valuestring,
            &expiry_end,
            10);

    if (expires_seconds == 0UL ||
        expiry_end == expires_in->valuestring ||
        (expiry_end != NULL &&
         *expiry_end != '\0') ||
        expires_seconds >
            (unsigned long)(INT64_MAX / 1000LL))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (strlen(id_token->valuestring) >=
            sizeof(s_id_token) ||
        strlen(refresh_token->valuestring) >=
            sizeof(s_refresh_token) ||
        strlen(uid_value) >=
            sizeof(s_user_uid))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_config.expected_uid != NULL &&
        s_config.expected_uid[0] != '\0' &&
        strcmp(
            uid_value,
            s_config.expected_uid) != 0)
    {
        ESP_LOGE(
            TAG,
            "Firebase user UID does not match configured device UID");

        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (xSemaphoreTake(
            s_state_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        cJSON_Delete(root);
        return ESP_ERR_TIMEOUT;
    }

    memcpy(
        s_id_token,
        id_token->valuestring,
        strlen(id_token->valuestring) + 1U);

    memcpy(
        s_refresh_token,
        refresh_token->valuestring,
        strlen(refresh_token->valuestring) + 1U);

    memcpy(
        s_user_uid,
        uid_value,
        strlen(uid_value) + 1U);

    s_status.token_expiry_uptime_ms =
        firebase_auth_get_time_ms() +
        ((int64_t)expires_seconds * 1000LL);

    s_status.has_id_token = true;
    s_status.has_refresh_token = true;
    firebase_auth_increment_generation_locked();

    xSemaphoreGive(s_state_mutex);

    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t firebase_auth_sign_in(void)
{
    if (xSemaphoreTake(
            s_state_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_status.state =
        FIREBASE_AUTH_STATE_SIGNING_IN;

    xSemaphoreGive(s_state_mutex);

    int url_length =
        snprintf(
            s_url_buffer,
            sizeof(s_url_buffer),
            FIREBASE_SIGN_IN_URL_FORMAT,
            s_config.api_key);

    if (url_length < 0 ||
        url_length >= (int)sizeof(s_url_buffer))
    {
        firebase_auth_zeroize(
            s_url_buffer,
            sizeof(s_url_buffer));
        firebase_auth_record_local_failure(
            ESP_ERR_INVALID_SIZE);

        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *request_root =
        cJSON_CreateObject();

    if (request_root == NULL)
    {
        firebase_auth_zeroize(
            s_url_buffer,
            sizeof(s_url_buffer));
        firebase_auth_record_local_failure(
            ESP_ERR_NO_MEM);

        return ESP_ERR_NO_MEM;
    }

    bool request_valid =
        cJSON_AddStringToObject(
            request_root,
            "email",
            s_config.email) != NULL &&
        cJSON_AddStringToObject(
            request_root,
            "password",
            s_config.password) != NULL &&
        cJSON_AddBoolToObject(
            request_root,
            "returnSecureToken",
            true) != NULL;

    if (!request_valid)
    {
        cJSON_Delete(request_root);
        firebase_auth_zeroize(
            s_url_buffer,
            sizeof(s_url_buffer));
        firebase_auth_record_local_failure(
            ESP_ERR_NO_MEM);

        return ESP_ERR_NO_MEM;
    }

    char *request_body =
        cJSON_PrintUnformatted(request_root);

    cJSON_Delete(request_root);

    if (request_body == NULL)
    {
        firebase_auth_zeroize(
            s_url_buffer,
            sizeof(s_url_buffer));
        firebase_auth_record_local_failure(
            ESP_ERR_NO_MEM);

        return ESP_ERR_NO_MEM;
    }

    int http_status = 0;

    esp_err_t result =
        firebase_auth_http_post(
            s_url_buffer,
            "application/json",
            request_body,
            &http_status);

    firebase_auth_zeroize(
        request_body,
        strlen(request_body));
    cJSON_free(request_body);

    if (result != ESP_OK)
    {
        firebase_auth_zeroize(
            s_response_buffer,
            sizeof(s_response_buffer));

        if (xSemaphoreTake(
                s_state_mutex,
                portMAX_DELAY) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        s_status.state =
            FIREBASE_AUTH_STATE_NETWORK_ERROR;
        s_status.last_error = result;
        s_status.last_http_status = http_status;
        s_status.failed_request_count++;

        xSemaphoreGive(s_state_mutex);

        return result;
    }

    if (http_status < 200 ||
        http_status >= 300)
    {
        firebase_auth_log_server_error();
        firebase_auth_zeroize(
            s_response_buffer,
            sizeof(s_response_buffer));

        if (xSemaphoreTake(
                s_state_mutex,
                portMAX_DELAY) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        s_status.state =
            (http_status >= 400 &&
             http_status < 500)
                ? FIREBASE_AUTH_STATE_CREDENTIAL_ERROR
                : FIREBASE_AUTH_STATE_NETWORK_ERROR;

        s_status.last_error = ESP_FAIL;
        s_status.last_http_status = http_status;
        s_status.failed_request_count++;

        xSemaphoreGive(s_state_mutex);

        return ESP_FAIL;
    }

    result =
        firebase_auth_parse_tokens(
            false);
    firebase_auth_zeroize(
        s_response_buffer,
        sizeof(s_response_buffer));

    if (result != ESP_OK)
    {
        if (xSemaphoreTake(
                s_state_mutex,
                portMAX_DELAY) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        s_status.state =
            FIREBASE_AUTH_STATE_CREDENTIAL_ERROR;
        s_status.last_error = result;
        s_status.last_http_status = http_status;
        s_status.failed_request_count++;

        xSemaphoreGive(s_state_mutex);

        return result;
    }

    if (xSemaphoreTake(
            s_state_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_status.state =
        FIREBASE_AUTH_STATE_AUTHENTICATED;
    s_status.last_error = ESP_OK;
    s_status.last_http_status = http_status;
    s_status.successful_sign_in_count++;

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(
        TAG,
        "Firebase Authentication sign-in successful");

    return ESP_OK;
}

static esp_err_t firebase_auth_url_encode(
    const char *input,
    char *output,
    size_t output_size)
{
    if (input == NULL ||
        output == NULL ||
        output_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    static const char hex[] =
        "0123456789ABCDEF";

    size_t output_index = 0U;

    for (size_t input_index = 0U;
         input[input_index] != '\0';
         input_index++)
    {
        unsigned char value =
            (unsigned char)input[input_index];

        bool is_unreserved =
            isalnum(value) ||
            value == '-' ||
            value == '_' ||
            value == '.' ||
            value == '~';

        size_t required_size =
            is_unreserved ? 1U : 3U;

        if (output_index + required_size >=
            output_size)
        {
            return ESP_ERR_INVALID_SIZE;
        }

        if (is_unreserved)
        {
            output[output_index++] =
                (char)value;
        }
        else
        {
            output[output_index++] = '%';
            output[output_index++] =
                hex[(value >> 4) & 0x0F];
            output[output_index++] =
                hex[value & 0x0F];
        }
    }

    output[output_index] = '\0';

    return ESP_OK;
}

static esp_err_t firebase_auth_refresh(void)
{
    if (xSemaphoreTake(
            s_state_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    if (s_refresh_token[0] == '\0')
    {
        xSemaphoreGive(s_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(
        s_refresh_token_snapshot,
        s_refresh_token,
        sizeof(s_refresh_token_snapshot));

    s_status.state =
        FIREBASE_AUTH_STATE_REFRESHING;

    xSemaphoreGive(s_state_mutex);

    char encoded_refresh_token[
        FIREBASE_AUTH_REQUEST_BUFFER_SIZE];

    esp_err_t result =
        firebase_auth_url_encode(
            s_refresh_token_snapshot,
            encoded_refresh_token,
            sizeof(encoded_refresh_token));

    firebase_auth_zeroize(
        s_refresh_token_snapshot,
        sizeof(s_refresh_token_snapshot));

    if (result != ESP_OK)
    {
        firebase_auth_zeroize(
            encoded_refresh_token,
            sizeof(encoded_refresh_token));
        firebase_auth_zeroize(
            s_request_buffer,
            sizeof(s_request_buffer));
        firebase_auth_zeroize(
            s_url_buffer,
            sizeof(s_url_buffer));
        firebase_auth_record_local_failure(
            result);

        return result;
    }

    int body_length =
        snprintf(
            s_request_buffer,
            sizeof(s_request_buffer),
            "grant_type=refresh_token&refresh_token=%s",
            encoded_refresh_token);

    firebase_auth_zeroize(
        encoded_refresh_token,
        sizeof(encoded_refresh_token));

    if (body_length < 0 ||
        body_length >= (int)sizeof(s_request_buffer))
    {
        firebase_auth_zeroize(
            s_request_buffer,
            sizeof(s_request_buffer));
        firebase_auth_zeroize(
            s_url_buffer,
            sizeof(s_url_buffer));
        firebase_auth_record_local_failure(
            ESP_ERR_INVALID_SIZE);

        return ESP_ERR_INVALID_SIZE;
    }

    int url_length =
        snprintf(
            s_url_buffer,
            sizeof(s_url_buffer),
            FIREBASE_REFRESH_URL_FORMAT,
            s_config.api_key);

    if (url_length < 0 ||
        url_length >= (int)sizeof(s_url_buffer))
    {
        firebase_auth_zeroize(
            s_request_buffer,
            sizeof(s_request_buffer));
        firebase_auth_zeroize(
            s_url_buffer,
            sizeof(s_url_buffer));
        firebase_auth_record_local_failure(
            ESP_ERR_INVALID_SIZE);

        return ESP_ERR_INVALID_SIZE;
    }

    int http_status = 0;

    result =
        firebase_auth_http_post(
            s_url_buffer,
            "application/x-www-form-urlencoded",
            s_request_buffer,
            &http_status);

    firebase_auth_zeroize(
        s_request_buffer,
        sizeof(s_request_buffer));

    if (result != ESP_OK)
    {
        firebase_auth_zeroize(
            s_response_buffer,
            sizeof(s_response_buffer));

        if (xSemaphoreTake(
                s_state_mutex,
                portMAX_DELAY) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        s_status.state =
            FIREBASE_AUTH_STATE_NETWORK_ERROR;
        s_status.last_error = result;
        s_status.last_http_status = http_status;
        s_status.failed_request_count++;

        xSemaphoreGive(s_state_mutex);

        return result;
    }

    if (http_status < 200 ||
        http_status >= 300)
    {
        firebase_auth_log_server_error();
        firebase_auth_zeroize(
            s_response_buffer,
            sizeof(s_response_buffer));

        if (xSemaphoreTake(
                s_state_mutex,
                portMAX_DELAY) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        s_status.state =
            (http_status >= 400 &&
             http_status < 500)
                ? FIREBASE_AUTH_STATE_CREDENTIAL_ERROR
                : FIREBASE_AUTH_STATE_NETWORK_ERROR;

        s_status.last_error = ESP_FAIL;
        s_status.last_http_status = http_status;
        s_status.failed_request_count++;

        xSemaphoreGive(s_state_mutex);

        return ESP_FAIL;
    }

    result =
        firebase_auth_parse_tokens(
            true);
    firebase_auth_zeroize(
        s_response_buffer,
        sizeof(s_response_buffer));

    if (result != ESP_OK)
    {
        if (xSemaphoreTake(
                s_state_mutex,
                portMAX_DELAY) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;
        }

        s_status.state =
            FIREBASE_AUTH_STATE_CREDENTIAL_ERROR;
        s_status.last_error = result;
        s_status.last_http_status = http_status;
        s_status.failed_request_count++;

        xSemaphoreGive(s_state_mutex);

        return result;
    }

    if (xSemaphoreTake(
            s_state_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_status.state =
        FIREBASE_AUTH_STATE_AUTHENTICATED;
    s_status.last_error = ESP_OK;
    s_status.last_http_status = http_status;
    s_status.successful_refresh_count++;

    xSemaphoreGive(s_state_mutex);

    ESP_LOGI(
        TAG,
        "Firebase ID token refreshed successfully");

    return ESP_OK;
}

/* Functions ---------------------------------------------------------------- */
esp_err_t firebase_auth_init(
    const firebase_auth_config_t *config)
{
    if (config == NULL ||
        config->api_key == NULL ||
        config->email == NULL ||
        config->password == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (config->api_key[0] == '\0' ||
        config->email[0] == '\0' ||
        config->password[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result =
        firebase_auth_copy_string(
            s_api_key,
            sizeof(s_api_key),
            config->api_key);

    if (result == ESP_OK)
    {
        result =
            firebase_auth_copy_string(
                s_email,
                sizeof(s_email),
                config->email);
    }

    if (result == ESP_OK)
    {
        result =
            firebase_auth_copy_string(
                s_password,
                sizeof(s_password),
                config->password);
    }

    if (result == ESP_OK &&
        config->expected_uid != NULL)
    {
        result =
            firebase_auth_copy_string(
                s_expected_uid,
                sizeof(s_expected_uid),
                config->expected_uid);
    }

    if (result != ESP_OK)
    {
        return result;
    }

    s_operation_mutex =
        xSemaphoreCreateMutex();

    if (s_operation_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_state_mutex =
        xSemaphoreCreateMutex();

    if (s_state_mutex == NULL)
    {
        vSemaphoreDelete(s_operation_mutex);
        s_operation_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(
        &s_status,
        0,
        sizeof(s_status));

    memset(
        s_id_token,
        0,
        sizeof(s_id_token));

    memset(
        s_refresh_token,
        0,
        sizeof(s_refresh_token));

    memset(
        s_user_uid,
        0,
        sizeof(s_user_uid));

    memset(
        s_refresh_token_snapshot,
        0,
        sizeof(s_refresh_token_snapshot));

    s_config.api_key = s_api_key;
    s_config.email = s_email;
    s_config.password = s_password;
    s_config.expected_uid =
        s_expected_uid[0] != '\0'
            ? s_expected_uid
            : NULL;

    s_config.refresh_margin_seconds =
        config->refresh_margin_seconds == 0U
            ? FIREBASE_AUTH_DEFAULT_MARGIN_SECONDS
            : config->refresh_margin_seconds;

    s_status.state =
        FIREBASE_AUTH_STATE_INITIALIZED;

    s_status.last_error = ESP_OK;
    s_status.token_generation = 1U;

    s_is_initialized = true;

    ESP_LOGI(
        TAG,
        "Firebase Authentication initialized");

    return ESP_OK;
}

esp_err_t firebase_auth_get_valid_id_token(
    char *token_buffer,
    size_t token_buffer_size)
{
    if (token_buffer == NULL ||
        token_buffer_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    token_buffer[0] = '\0';

    if (!s_is_initialized ||
        s_operation_mutex == NULL ||
        s_state_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_operation_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    bool token_valid = false;
    bool has_refresh_token = false;

    if (xSemaphoreTake(
            s_state_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        xSemaphoreGive(s_operation_mutex);
        return ESP_ERR_TIMEOUT;
    }

    token_valid =
        firebase_auth_token_is_valid_locked();
    has_refresh_token =
        (s_refresh_token[0] != '\0');

    if (token_valid)
    {
        result =
            firebase_auth_copy_string(
                token_buffer,
                token_buffer_size,
                s_id_token);
    }

    xSemaphoreGive(s_state_mutex);

    if (!token_valid)
    {
        if (has_refresh_token)
        {
            result =
                firebase_auth_refresh();

            /*
             * Fall back to full sign-in only when the refresh credential is
             * rejected. Do not perform a second network call after an
             * ordinary transport failure.
             */
            firebase_auth_state_t auth_state =
                FIREBASE_AUTH_STATE_UNINITIALIZED;

            if (xSemaphoreTake(
                    s_state_mutex,
                    portMAX_DELAY) == pdTRUE)
            {
                auth_state = s_status.state;
                xSemaphoreGive(s_state_mutex);
            }

            if (result != ESP_OK &&
                auth_state ==
                    FIREBASE_AUTH_STATE_CREDENTIAL_ERROR)
            {
                if (xSemaphoreTake(
                        s_state_mutex,
                        portMAX_DELAY) != pdTRUE)
                {
                    result = ESP_ERR_TIMEOUT;
                }
                else
                {
                    firebase_auth_zeroize(
                        s_id_token,
                        sizeof(s_id_token));
                    firebase_auth_zeroize(
                        s_refresh_token,
                        sizeof(s_refresh_token));
                    s_status.has_id_token = false;
                    s_status.has_refresh_token = false;
                    s_status.token_expiry_uptime_ms = 0;
                    firebase_auth_increment_generation_locked();
                    xSemaphoreGive(s_state_mutex);

                    result =
                        firebase_auth_sign_in();
                }
            }
        }
        else
        {
            result =
                firebase_auth_sign_in();
        }

        if (result == ESP_OK)
        {
            if (xSemaphoreTake(
                    s_state_mutex,
                    portMAX_DELAY) != pdTRUE)
            {
                result = ESP_ERR_TIMEOUT;
            }
            else
            {
                result =
                    firebase_auth_copy_string(
                        token_buffer,
                        token_buffer_size,
                        s_id_token);

                xSemaphoreGive(s_state_mutex);
            }
        }
    }

    xSemaphoreGive(s_operation_mutex);

    return result;
}

esp_err_t firebase_auth_invalidate_id_token(void)
{
    if (!s_is_initialized ||
        s_state_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_state_mutex,
            pdMS_TO_TICKS(
                FIREBASE_AUTH_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    firebase_auth_zeroize(
        s_id_token,
        sizeof(s_id_token));
    s_status.has_id_token = false;
    s_status.token_expiry_uptime_ms = 0;
    s_status.state =
        FIREBASE_AUTH_STATE_INITIALIZED;
    firebase_auth_increment_generation_locked();

    xSemaphoreGive(s_state_mutex);

    return ESP_OK;
}

esp_err_t firebase_auth_get_status(
    firebase_auth_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_is_initialized ||
        s_state_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_state_mutex,
            pdMS_TO_TICKS(
                FIREBASE_AUTH_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    *status = s_status;

    xSemaphoreGive(s_state_mutex);

    return ESP_OK;
}
