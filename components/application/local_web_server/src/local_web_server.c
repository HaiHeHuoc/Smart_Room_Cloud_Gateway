#include "local_web_server.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"

#include "app_log.h"
#include "local_web_path_policy.h"
#include "sd_card_manager.h"

#define LOCAL_WEB_HTTP_STACK_SIZE_BYTES 6144U
#define LOCAL_WEB_HTTP_MAX_OPEN_SOCKETS 2U
#define LOCAL_WEB_HTTP_MAX_URI_LEN 640U
#define LOCAL_WEB_RESPONSE_CHUNK_SIZE 512U
#define LOCAL_WEB_QUERY_BUFFER_SIZE \
    ((LOCAL_WEB_LOGICAL_PATH_MAX_LEN * 3U) + 16U)

static const char *const TAG = "local_web_server";

extern const unsigned char local_web_index_html_start[]
    asm("_binary_web_index_html_start");
extern const unsigned char local_web_index_html_end[]
    asm("_binary_web_index_html_end");

static httpd_handle_t s_server;
static bool s_initialized;

/* HTTPD invokes URI handlers serially in its one server task. This avoids
 * placing the bounded list (about 2.5 KiB) on that task's call stack. */
static sd_card_manager_directory_listing_t s_listing;
static char s_response_chunk[LOCAL_WEB_RESPONSE_CHUNK_SIZE];

static esp_err_t local_web_root_get(httpd_req_t *request);
static esp_err_t local_web_storage_status_get(httpd_req_t *request);
static esp_err_t local_web_storage_list_get(httpd_req_t *request);
static esp_err_t local_web_send_error(
    httpd_req_t *request,
    const char *http_status,
    const char *error_code);
static esp_err_t local_web_send_json_escaped(
    httpd_req_t *request,
    const char *value);
static const char *local_web_sd_state_name(sd_card_manager_state_t state);
static const char *local_web_entry_type_name(
    sd_card_manager_directory_entry_type_t type);
static esp_err_t local_web_send_list_result(httpd_req_t *request);
static esp_err_t local_web_register_routes(httpd_handle_t server);

esp_err_t local_web_server_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_server = NULL;
    s_listing = (sd_card_manager_directory_listing_t){0};
    s_initialized = true;
    return ESP_OK;
}

esp_err_t local_web_server_start(void)
{
    if (!s_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server != NULL)
    {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = LOCAL_WEB_HTTP_STACK_SIZE_BYTES;
    config.max_open_sockets = LOCAL_WEB_HTTP_MAX_OPEN_SOCKETS;
    config.max_uri_handlers = 3U;
    config.max_uri_len = LOCAL_WEB_HTTP_MAX_URI_LEN;
    config.recv_wait_timeout = 5U;
    config.send_wait_timeout = 5U;
    config.lru_purge_enable = true;

    esp_err_t result = httpd_start(&s_server, &config);
    if (result != ESP_OK)
    {
        APP_LOGW(TAG, LOCAL_WEB_HTTP_START_FAILED_7F455BC2,
                 "Local Web server start failed: %s", esp_err_to_name(result));
        s_server = NULL;
        return result;
    }

    result = local_web_register_routes(s_server);
    if (result != ESP_OK)
    {
        APP_LOGW(TAG, LOCAL_WEB_ROUTE_REGISTER_FAILED_2742B577,
                 "Local Web route registration failed: %s", esp_err_to_name(result));
        (void)httpd_stop(s_server);
        s_server = NULL;
        return result;
    }

    APP_LOGI(TAG, LOCAL_WEB_SERVER_STARTED_EE977447,
             "Local read-only Storage Web UI started on HTTP port %u",
             (unsigned)config.server_port);
    return ESP_OK;
}

static esp_err_t local_web_root_get(httpd_req_t *request)
{
    const size_t html_size =
        (size_t)(local_web_index_html_end - local_web_index_html_start);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(
        request, (const char *)local_web_index_html_start, html_size);
}

static esp_err_t local_web_storage_status_get(httpd_req_t *request)
{
    sd_card_manager_status_t sd_status = {0};
    const esp_err_t status_result = sd_card_manager_get_status(&sd_status);
    if (status_result != ESP_OK)
    {
        return local_web_send_error(request, "503 Service Unavailable", "storage_unavailable");
    }

    sd_card_manager_filesystem_usage_t usage = {0};
    const bool available =
        (sd_status.state == SD_CARD_MANAGER_STATE_READY) &&
        (sd_card_manager_get_filesystem_usage(&usage) == ESP_OK);

    const int written = snprintf(
        s_response_chunk, sizeof(s_response_chunk),
        "{\"ok\":true,\"available\":%s,\"state\":\"%s\","
        "\"total_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64
        ",\"free_bytes\":%" PRIu64 "}",
        available ? "true" : "false", local_web_sd_state_name(sd_status.state),
        usage.total_bytes, usage.used_bytes, usage.free_bytes);
    if ((written < 0) || (written >= (int)sizeof(s_response_chunk)))
    {
        return local_web_send_error(request, "500 Internal Server Error", "response_too_large");
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_storage_list_get(httpd_req_t *request)
{
    const size_t query_length = httpd_req_get_url_query_len(request);
    char encoded_path[LOCAL_WEB_QUERY_BUFFER_SIZE] = "/";
    if (query_length > 0U)
    {
        if (query_length >= LOCAL_WEB_QUERY_BUFFER_SIZE)
        {
            return local_web_send_error(request, "400 Bad Request", "malformed_request");
        }

        char query[LOCAL_WEB_QUERY_BUFFER_SIZE] = {0};
        if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)
        {
            return local_web_send_error(request, "400 Bad Request", "malformed_request");
        }

        const esp_err_t query_result = httpd_query_key_value(
            query, "path", encoded_path, sizeof(encoded_path));
        if ((query_result != ESP_OK) && (query_result != ESP_ERR_NOT_FOUND))
        {
            return local_web_send_error(request, "400 Bad Request", "malformed_request");
        }
    }

    char logical_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_path_policy_normalize(
            encoded_path, logical_path, sizeof(logical_path)) != ESP_OK)
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_path");
    }

    const esp_err_t list_result =
        sd_card_manager_list_directory(logical_path, &s_listing);
    if (list_result == ESP_ERR_INVALID_STATE)
    {
        return local_web_send_error(request, "503 Service Unavailable", "storage_unavailable");
    }
    if (list_result == ESP_ERR_NOT_FOUND)
    {
        return local_web_send_error(request, "404 Not Found", "directory_not_found");
    }
    if (list_result == ESP_ERR_NOT_SUPPORTED)
    {
        return local_web_send_error(request, "400 Bad Request", "not_a_directory");
    }
    if (list_result != ESP_OK)
    {
        return local_web_send_error(request, "500 Internal Server Error", "directory_read_failed");
    }

    return local_web_send_list_result(request);
}

static esp_err_t local_web_send_list_result(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");

    if (httpd_resp_sendstr_chunk(request, "{\"ok\":true,\"path\":\"") != ESP_OK ||
        local_web_send_json_escaped(request, s_listing.path) != ESP_OK ||
        httpd_resp_sendstr_chunk(request, "\",\"entries\":[") != ESP_OK)
    {
        return ESP_FAIL;
    }

    for (uint16_t index = 0U; index < s_listing.entry_count; index++)
    {
        const sd_card_manager_directory_entry_t *const entry =
            &s_listing.entries[index];
        const int prefix_length = snprintf(
            s_response_chunk, sizeof(s_response_chunk),
            "%s{\"name\":\"", (index == 0U) ? "" : ",");
        if ((prefix_length < 0) ||
            (prefix_length >= (int)sizeof(s_response_chunk)) ||
            (httpd_resp_send_chunk(request, s_response_chunk, prefix_length) != ESP_OK) ||
            (local_web_send_json_escaped(request, entry->name) != ESP_OK))
        {
            return ESP_FAIL;
        }

        const int suffix_length = snprintf(
            s_response_chunk, sizeof(s_response_chunk),
            "\",\"type\":\"%s\",\"size_bytes\":%" PRIu64 "}",
            local_web_entry_type_name(entry->type), entry->size_bytes);
        if ((suffix_length < 0) ||
            (suffix_length >= (int)sizeof(s_response_chunk)) ||
            (httpd_resp_send_chunk(request, s_response_chunk, suffix_length) != ESP_OK))
        {
            return ESP_FAIL;
        }
    }

    const int end_length = snprintf(
        s_response_chunk, sizeof(s_response_chunk),
        "],\"truncated\":%s,\"unsupported_entries\":%u}",
        s_listing.truncated ? "true" : "false",
        (unsigned)s_listing.unsupported_entry_count);
    if ((end_length < 0) || (end_length >= (int)sizeof(s_response_chunk)) ||
        (httpd_resp_send_chunk(request, s_response_chunk, end_length) != ESP_OK))
    {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(request, NULL, 0U);
}

static esp_err_t local_web_send_error(
    httpd_req_t *request,
    const char *http_status,
    const char *error_code)
{
    httpd_resp_set_status(request, http_status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const int written = snprintf(
        s_response_chunk, sizeof(s_response_chunk),
        "{\"ok\":false,\"error\":\"%s\"}", error_code);
    return ((written < 0) || (written >= (int)sizeof(s_response_chunk)))
               ? ESP_FAIL
               : httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_send_json_escaped(
    httpd_req_t *request,
    const char *value)
{
    if (value == NULL) return ESP_ERR_INVALID_ARG;
    size_t output_length = 0U;
    for (size_t index = 0U; value[index] != '\0'; index++)
    {
        const unsigned char character = (unsigned char)value[index];
        const char *escape = NULL;
        if (character == '"') escape = "\\\"";
        else if (character == '\\') escape = "\\\\";
        else if (character < 0x20U) escape = "?";

        if (escape != NULL)
        {
            const size_t escape_length = strlen(escape);
            if ((output_length + escape_length) >= sizeof(s_response_chunk))
            {
                if (httpd_resp_send_chunk(request, s_response_chunk, output_length) != ESP_OK)
                {
                    return ESP_FAIL;
                }
                output_length = 0U;
            }
            memcpy(&s_response_chunk[output_length], escape, escape_length);
            output_length += escape_length;
        }
        else
        {
            if ((output_length + 1U) >= sizeof(s_response_chunk))
            {
                if (httpd_resp_send_chunk(request, s_response_chunk, output_length) != ESP_OK)
                {
                    return ESP_FAIL;
                }
                output_length = 0U;
            }
            s_response_chunk[output_length++] = (char)character;
        }
    }
    return (output_length == 0U)
               ? ESP_OK
               : httpd_resp_send_chunk(request, s_response_chunk, output_length);
}

static const char *local_web_sd_state_name(sd_card_manager_state_t state)
{
    switch (state)
    {
        case SD_CARD_MANAGER_STATE_READY: return "ready";
        case SD_CARD_MANAGER_STATE_UNAVAILABLE: return "unavailable";
        case SD_CARD_MANAGER_STATE_RECOVERING: return "recovering";
        case SD_CARD_MANAGER_STATE_MOUNTING: return "mounting";
        case SD_CARD_MANAGER_STATE_RETRY_WAIT: return "retry_wait";
        case SD_CARD_MANAGER_STATE_INITIALIZING: return "initializing";
        case SD_CARD_MANAGER_STATE_UNINITIALIZED:
        default: return "uninitialized";
    }
}

static const char *local_web_entry_type_name(
    sd_card_manager_directory_entry_type_t type)
{
    switch (type)
    {
        case SD_CARD_MANAGER_DIRECTORY_ENTRY_DIRECTORY: return "directory";
        case SD_CARD_MANAGER_DIRECTORY_ENTRY_FILE: return "file";
        case SD_CARD_MANAGER_DIRECTORY_ENTRY_OTHER:
        default: return "other";
    }
}

static esp_err_t local_web_register_routes(httpd_handle_t server)
{
    static const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = local_web_root_get,
    };
    static const httpd_uri_t storage_status = {
        .uri = "/api/storage/status", .method = HTTP_GET,
        .handler = local_web_storage_status_get,
    };
    static const httpd_uri_t storage_list = {
        .uri = "/api/storage/list", .method = HTTP_GET,
        .handler = local_web_storage_list_get,
    };

    esp_err_t result = httpd_register_uri_handler(server, &root);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_list);
    return result;
}
