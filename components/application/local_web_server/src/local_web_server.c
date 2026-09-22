#include "local_web_server.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"

#include "app_log.h"
#include "app_gui.h"
#include "audio_manager.h"
#include "local_web_download.h"
#include "local_web_audio_policy.h"
#include "local_web_path_policy.h"
#include "sd_card_manager.h"
#include "smart_room_mcp_adapter.h"
#include "voice_assistant_playback_control.h"

#define LOCAL_WEB_HTTP_STACK_SIZE_BYTES 6144U
#define LOCAL_WEB_HTTP_MAX_OPEN_SOCKETS 2U
#define LOCAL_WEB_HTTP_MAX_URI_LEN 1280U
#define LOCAL_WEB_RESPONSE_CHUNK_SIZE 512U
#define LOCAL_WEB_TRANSFER_CHUNK_SIZE SD_CARD_MANAGER_TRANSFER_CHUNK_SIZE
#define LOCAL_WEB_QUERY_VALUE_SIZE \
    ((LOCAL_WEB_LOGICAL_PATH_MAX_LEN * 3U) + 1U)
#define LOCAL_WEB_QUERY_BUFFER_SIZE \
    ((LOCAL_WEB_LOGICAL_PATH_MAX_LEN * 3U * 2U) + 24U)

static const char *const TAG = "local_web_server";

extern const unsigned char local_web_index_html_start[]
    asm("_binary_index_html_start");
extern const unsigned char local_web_index_html_end[]
    asm("_binary_index_html_end");

static httpd_handle_t s_server;
static bool s_initialized;

/* HTTPD invokes URI handlers serially in one server task. Keep bounded
 * response, transfer, and query buffers out of its 6 KiB stack. */
static sd_card_manager_directory_listing_t s_listing;
static char s_response_chunk[LOCAL_WEB_RESPONSE_CHUNK_SIZE];
static uint8_t s_transfer_chunk[LOCAL_WEB_TRANSFER_CHUNK_SIZE];
static char s_download_content_disposition[
    LOCAL_WEB_DOWNLOAD_CONTENT_DISPOSITION_MAX_LEN];
static char s_query_buffer[LOCAL_WEB_QUERY_BUFFER_SIZE];
static char s_query_value[LOCAL_WEB_QUERY_VALUE_SIZE];

static esp_err_t local_web_root_get(httpd_req_t *request);
static esp_err_t local_web_storage_status_get(httpd_req_t *request);
static esp_err_t local_web_storage_list_get(httpd_req_t *request);
static esp_err_t local_web_storage_download_get(httpd_req_t *request);
static esp_err_t local_web_storage_upload_post(httpd_req_t *request);
static esp_err_t local_web_storage_delete_post(httpd_req_t *request);
static esp_err_t local_web_storage_mkdir_post(httpd_req_t *request);
static esp_err_t local_web_storage_rmdir_post(httpd_req_t *request);
static esp_err_t local_web_storage_rename_post(httpd_req_t *request);
static esp_err_t local_web_audio_status_get(httpd_req_t *request);
static esp_err_t local_web_audio_tracks_get(httpd_req_t *request);
static esp_err_t local_web_audio_play_post(httpd_req_t *request);
static esp_err_t local_web_audio_control_post(httpd_req_t *request);
static esp_err_t local_web_audio_volume_post(httpd_req_t *request);
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
static esp_err_t local_web_get_normalized_query_path(
    httpd_req_t *request,
    const char *key,
    char *logical_path,
    size_t logical_path_size);
static esp_err_t local_web_send_storage_result(
    httpd_req_t *request,
    esp_err_t result,
    const char *not_found_error);
static void local_web_publish_storage_status(uint8_t progress_percent,
                                             esp_err_t last_error);
static esp_err_t local_web_register_routes(httpd_handle_t server);

static const char *local_web_audio_state_name(
    audio_manager_playback_control_state_t state)
{
    switch (state) {
        case AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE: return "idle";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING: return "starting";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING: return "playing";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING: return "pausing";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED: return "paused";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING: return "resuming";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_STOPPING: return "stopping";
        case AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR: return "error";
        default: return "unknown";
    }
}

static const char *local_web_audio_source_name(
    audio_manager_playback_source_t source)
{
    return audio_manager_playback_source_to_string(source);
}

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
    config.max_uri_handlers = 14U;
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
             "Local Storage Web UI started on HTTP port %u",
             (unsigned)config.server_port);
    local_web_publish_storage_status(0U, ESP_OK);
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
    const bool storage_ready =
        sd_status.state == SD_CARD_MANAGER_STATE_READY;
    if (storage_ready &&
        (sd_card_manager_get_filesystem_usage(&usage) != ESP_OK))
    {
        return local_web_send_error(
            request, "503 Service Unavailable", "storage_usage_unavailable");
    }

    const int written = snprintf(
        s_response_chunk, sizeof(s_response_chunk),
        "{\"ok\":true,\"available\":%s,\"state\":\"%s\","
        "\"total_bytes\":%" PRIu64 ",\"used_bytes\":%" PRIu64
        ",\"free_bytes\":%" PRIu64 "}",
        storage_ready ? "true" : "false", local_web_sd_state_name(sd_status.state),
        usage.total_bytes, usage.used_bytes, usage.free_bytes);
    if ((written < 0) || (written >= (int)sizeof(s_response_chunk)))
    {
        return local_web_send_error(request, "500 Internal Server Error", "response_too_large");
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_audio_status_get(httpd_req_t *request)
{
    audio_manager_playback_status_t playback = {0};
    uint32_t volume = 0U;
    const esp_err_t playback_ret = voice_assistant_playback_get_status(&playback);
    const esp_err_t volume_ret = audio_manager_get_playback_volume_percent(&volume);
    if ((playback_ret == ESP_ERR_INVALID_STATE) ||
        (volume_ret == ESP_ERR_INVALID_STATE)) {
        return local_web_send_error(request, "503 Service Unavailable", "audio_unavailable");
    }
    if ((playback_ret != ESP_OK) || (volume_ret != ESP_OK)) {
        return local_web_send_error(request, "500 Internal Server Error", "audio_status_failed");
    }
    const int written = snprintf(
        s_response_chunk, sizeof(s_response_chunk),
        "{\"ok\":true,\"state\":\"%s\",\"source\":\"%s\","
        "\"resumable\":%s,\"generation\":%" PRIu32 ","
        "\"position_frames\":%" PRIu64 ",\"total_frames\":%" PRIu64 ","
        "\"position_granularity_frames\":%" PRIu32 ",\"volume_percent\":%" PRIu32 "}",
        local_web_audio_state_name(playback.state),
        local_web_audio_source_name(playback.source),
        playback.resumable ? "true" : "false", playback.generation,
        playback.position_frames, playback.total_frames,
        playback.position_granularity_frames, volume);
    if ((written < 0) || (written >= (int)sizeof(s_response_chunk))) {
        return local_web_send_error(request, "500 Internal Server Error", "response_too_large");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_audio_tracks_get(httpd_req_t *request)
{
    smart_room_audio_catalog_t catalog = {0};
    const esp_err_t ret = smart_room_mcp_adapter_audio_catalog_get(&catalog);
    if (ret == ESP_ERR_TIMEOUT) {
        return local_web_send_error(request, "503 Service Unavailable", "catalog_busy");
    }
    if (ret != ESP_OK) {
        return local_web_send_error(request, "503 Service Unavailable", "catalog_unavailable");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (httpd_resp_sendstr_chunk(request, "{\"ok\":true,\"tracks\":[") != ESP_OK) {
        return ESP_FAIL;
    }
    for (uint8_t index = 0U; index < catalog.track_count; ++index) {
        const char *prefix = (index == 0U) ? "{\"id\":\"" : ",{\"id\":\"";
        if ((httpd_resp_sendstr_chunk(request, prefix) != ESP_OK) ||
            (local_web_send_json_escaped(request, catalog.tracks[index].id) != ESP_OK) ||
            (httpd_resp_sendstr_chunk(request, "\",\"name\":\"") != ESP_OK) ||
            (local_web_send_json_escaped(request, catalog.tracks[index].name) != ESP_OK)) {
            return ESP_FAIL;
        }
        if ((httpd_resp_sendstr_chunk(request, "\",\"filename\":\"") != ESP_OK) ||
            (local_web_send_json_escaped(request, catalog.tracks[index].filename) != ESP_OK)) {
            return ESP_FAIL;
        }
        const int written = snprintf(s_response_chunk, sizeof(s_response_chunk),
                                     "\",\"size_bytes\":%" PRIu64 "}",
                                     catalog.tracks[index].size_bytes);
        if ((written < 0) || (written >= (int)sizeof(s_response_chunk)) ||
            (httpd_resp_send_chunk(request, s_response_chunk, written) != ESP_OK)) {
            return ESP_FAIL;
        }
    }
    const int written = snprintf(s_response_chunk, sizeof(s_response_chunk),
                                 "],\"truncated\":%s}",
                                 catalog.truncated ? "true" : "false");
    return ((written < 0) || (written >= (int)sizeof(s_response_chunk)) ||
            (httpd_resp_send_chunk(request, s_response_chunk, written) != ESP_OK))
               ? ESP_FAIL : httpd_resp_send_chunk(request, NULL, 0U);
}

static bool local_web_get_query_value(httpd_req_t *request, const char *key)
{
    return (request != NULL) && (key != NULL) &&
        (httpd_req_get_url_query_len(request) < sizeof(s_query_buffer)) &&
        (httpd_req_get_url_query_str(request, s_query_buffer,
                                     sizeof(s_query_buffer)) == ESP_OK) &&
        (httpd_query_key_value(s_query_buffer, key, s_query_value,
                               sizeof(s_query_value)) == ESP_OK);
}

static esp_err_t local_web_audio_play_post(httpd_req_t *request)
{
    if (!local_web_get_query_value(request, "track_id")) {
        return local_web_send_error(request, "400 Bad Request", "invalid_request");
    }
    smart_room_audio_catalog_play_result_t result = {0};
    const esp_err_t ret = smart_room_mcp_adapter_audio_catalog_play(
        s_query_value, &result);
    if (ret != ESP_OK) {
        return local_web_send_error(request, "503 Service Unavailable", "catalog_unavailable");
    }
    const char *error = (result.outcome == SMART_ROOM_AUDIO_CATALOG_PLAY_NOT_FOUND)
                            ? "track_not_found"
                            : (result.outcome == SMART_ROOM_AUDIO_CATALOG_PLAY_INVALID_REQUEST)
                                  ? "invalid_request"
                                  : (result.outcome == SMART_ROOM_AUDIO_CATALOG_PLAY_STORAGE_UNAVAILABLE)
                                        ? "storage_unavailable"
                                        : (result.outcome == SMART_ROOM_AUDIO_CATALOG_PLAY_REJECTED)
                                              ? "playback_busy" : "catalog_unavailable";
    if (result.outcome != SMART_ROOM_AUDIO_CATALOG_PLAY_SUCCESS) {
        return local_web_send_error(request, "409 Conflict", error);
    }
    return httpd_resp_sendstr(request, "{\"ok\":true,\"accepted\":true,\"scheduled\":true}");
}

static esp_err_t local_web_audio_control_post(httpd_req_t *request)
{
    if (!local_web_get_query_value(request, "action")) {
        return local_web_send_error(request, "400 Bad Request", "invalid_request");
    }
    local_web_audio_action_t parsed_action;
    if (!local_web_audio_action_parse(s_query_value, &parsed_action)) {
        return local_web_send_error(request, "400 Bad Request", "unsupported_operation");
    }
    const voice_assistant_playback_action_t action =
        (parsed_action == LOCAL_WEB_AUDIO_ACTION_PAUSE)
            ? VOICE_ASSISTANT_PLAYBACK_ACTION_PAUSE
            : (parsed_action == LOCAL_WEB_AUDIO_ACTION_RESUME)
                  ? VOICE_ASSISTANT_PLAYBACK_ACTION_RESUME
                  : (parsed_action == LOCAL_WEB_AUDIO_ACTION_RESTART)
                        ? VOICE_ASSISTANT_PLAYBACK_ACTION_RESTART
                        : VOICE_ASSISTANT_PLAYBACK_ACTION_STOP;
    voice_assistant_playback_control_result_t result = {0};
    if (voice_assistant_playback_control(action, &result) != ESP_OK) {
        return local_web_send_error(request, "500 Internal Server Error", "internal_error");
    }
    if (!result.accepted) {
        return local_web_send_error(request, "409 Conflict", "invalid_state");
    }
    return httpd_resp_sendstr(request, "{\"ok\":true,\"accepted\":true}");
}

static esp_err_t local_web_audio_volume_post(httpd_req_t *request)
{
    if (!local_web_get_query_value(request, "percent")) {
        return local_web_send_error(request, "400 Bad Request", "invalid_request");
    }
    uint32_t value = 0U;
    if (!local_web_audio_volume_percent_parse(s_query_value, &value)) {
        return local_web_send_error(request, "400 Bad Request", "volume_out_of_range");
    }
    const esp_err_t ret = audio_manager_set_playback_volume_percent(value);
    if (ret == ESP_ERR_INVALID_STATE) {
        return local_web_send_error(request, "503 Service Unavailable", "audio_unavailable");
    }
    if (ret != ESP_OK) {
        return local_web_send_error(request, "400 Bad Request", "volume_out_of_range");
    }
    const int written = snprintf(s_response_chunk, sizeof(s_response_chunk),
                                 "{\"ok\":true,\"volume_percent\":%" PRIu32 "}", value);
    return ((written < 0) || (written >= (int)sizeof(s_response_chunk)))
               ? local_web_send_error(request, "500 Internal Server Error", "response_too_large")
               : httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_storage_list_get(httpd_req_t *request)
{
    char logical_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_get_normalized_query_path(
            request, "path", logical_path, sizeof(logical_path)) != ESP_OK)
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

static esp_err_t local_web_storage_download_get(httpd_req_t *request)
{
    char logical_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_get_normalized_query_path(
            request, "path", logical_path, sizeof(logical_path)) != ESP_OK ||
        (strcmp(logical_path, "/") == 0))
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_path");
    }

    sd_card_manager_transfer_info_t transfer = {0};
    const esp_err_t begin_result =
        sd_card_manager_download_begin(logical_path, &transfer);
    if (begin_result != ESP_OK)
    {
        return local_web_send_storage_result(request, begin_result, "file_not_found");
    }

    if (!local_web_download_content_disposition(
            logical_path, s_download_content_disposition,
            sizeof(s_download_content_disposition)))
    {
        (void)sd_card_manager_download_end(transfer.transfer_id);
        return local_web_send_error(
            request, "500 Internal Server Error", "download_filename_invalid");
    }

    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(
        request, "Content-Disposition", s_download_content_disposition);
    esp_err_t result = ESP_OK;
    for (;;)
    {
        size_t read_size = 0U;
        result = sd_card_manager_download_read(
            transfer.transfer_id, s_transfer_chunk, sizeof(s_transfer_chunk),
            &read_size);
        if (result != ESP_OK)
        {
            break;
        }
        if (read_size == 0U)
        {
            break;
        }
        if (httpd_resp_send_chunk(
                request, (const char *)s_transfer_chunk, read_size) != ESP_OK)
        {
            result = ESP_FAIL;
            break;
        }
    }

    const esp_err_t end_result = sd_card_manager_download_end(transfer.transfer_id);
    if ((result != ESP_OK) || (end_result != ESP_OK))
    {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(request, NULL, 0U);
}

static esp_err_t local_web_storage_upload_post(httpd_req_t *request)
{
    char logical_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_get_normalized_query_path(
            request, "path", logical_path, sizeof(logical_path)) != ESP_OK ||
        (strcmp(logical_path, "/") == 0) || (request->content_len <= 0))
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_request");
    }
    if ((uint64_t)request->content_len > SD_CARD_MANAGER_TRANSFER_MAX_BYTES)
    {
        return local_web_send_error(request, "413 Payload Too Large", "upload_too_large");
    }

    sd_card_manager_transfer_info_t transfer = {0};
    const esp_err_t begin_result = sd_card_manager_upload_begin(
        logical_path, (uint64_t)request->content_len, &transfer);
    if (begin_result != ESP_OK)
    {
        return local_web_send_storage_result(request, begin_result, "upload_failed");
    }

    size_t remaining = (size_t)request->content_len;
    esp_err_t result = ESP_OK;
    while (remaining > 0U)
    {
        const size_t requested = (remaining < sizeof(s_transfer_chunk)) ?
                                 remaining : sizeof(s_transfer_chunk);
        const int received = httpd_req_recv(
            request, (char *)s_transfer_chunk, requested);
        if (received <= 0)
        {
            result = ESP_FAIL;
            break;
        }
        result = sd_card_manager_upload_write(
            transfer.transfer_id, s_transfer_chunk, (size_t)received);
        if (result != ESP_OK)
        {
            break;
        }
        remaining -= (size_t)received;
    }

    if (result != ESP_OK)
    {
        sd_card_manager_upload_abort(transfer.transfer_id);
        return local_web_send_error(request, "500 Internal Server Error", "upload_interrupted");
    }
    result = sd_card_manager_upload_finish(transfer.transfer_id);
    if (result != ESP_OK)
    {
        return local_web_send_storage_result(request, result, "upload_failed");
    }
    local_web_publish_storage_status(100U, ESP_OK);
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static esp_err_t local_web_storage_delete_post(httpd_req_t *request)
{
    char logical_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_get_normalized_query_path(request, "path", logical_path,
                                            sizeof(logical_path)) != ESP_OK ||
        (strcmp(logical_path, "/") == 0))
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_path");
    }
    return local_web_send_storage_result(request,
        sd_card_manager_delete_file(logical_path), "file_not_found");
}

static esp_err_t local_web_storage_mkdir_post(httpd_req_t *request)
{
    char logical_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_get_normalized_query_path(request, "path", logical_path,
                                            sizeof(logical_path)) != ESP_OK ||
        (strcmp(logical_path, "/") == 0))
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_path");
    }
    return local_web_send_storage_result(request,
        sd_card_manager_make_directory(logical_path), "parent_not_found");
}

static esp_err_t local_web_storage_rmdir_post(httpd_req_t *request)
{
    char logical_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_get_normalized_query_path(request, "path", logical_path,
                                            sizeof(logical_path)) != ESP_OK ||
        (strcmp(logical_path, "/") == 0))
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_path");
    }
    return local_web_send_storage_result(request,
        sd_card_manager_remove_empty_directory(logical_path), "directory_not_found");
}

static esp_err_t local_web_storage_rename_post(httpd_req_t *request)
{
    char source[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    char destination[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_get_normalized_query_path(request, "src", source,
                                            sizeof(source)) != ESP_OK ||
        local_web_get_normalized_query_path(request, "dest", destination,
                                            sizeof(destination)) != ESP_OK ||
        (strcmp(source, "/") == 0) || (strcmp(destination, "/") == 0))
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_path");
    }
    return local_web_send_storage_result(request,
        sd_card_manager_rename_path(source, destination), "source_not_found");
}

static esp_err_t local_web_get_normalized_query_path(
    httpd_req_t *request,
    const char *key,
    char *logical_path,
    size_t logical_path_size)
{
    if ((request == NULL) || (key == NULL) || (logical_path == NULL) ||
        (httpd_req_get_url_query_len(request) >= LOCAL_WEB_QUERY_BUFFER_SIZE))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((httpd_req_get_url_query_str(
             request, s_query_buffer, sizeof(s_query_buffer)) != ESP_OK) ||
        (httpd_query_key_value(
             s_query_buffer, key, s_query_value,
             sizeof(s_query_value)) != ESP_OK))
    {
        return ESP_ERR_INVALID_ARG;
    }
    return local_web_path_policy_normalize(
        s_query_value, logical_path, logical_path_size);
}

static esp_err_t local_web_send_storage_result(
    httpd_req_t *request,
    esp_err_t result,
    const char *not_found_error)
{
    if (result == ESP_OK)
    {
        local_web_publish_storage_status(0U, ESP_OK);
        return httpd_resp_sendstr(request, "{\"ok\":true}");
    }
    if (result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_SIZE)
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_path");
    }
    if (result == ESP_ERR_TIMEOUT)
    {
        return local_web_send_error(request, "409 Conflict", "storage_busy");
    }
    if (result == ESP_ERR_INVALID_RESPONSE)
    {
        return local_web_send_error(request, "409 Conflict", "already_exists");
    }
    if (result == ESP_ERR_NOT_FINISHED)
    {
        return local_web_send_error(request, "409 Conflict", "directory_not_empty");
    }
    if (result == ESP_ERR_INVALID_STATE)
    {
        return local_web_send_error(request, "503 Service Unavailable", "storage_unavailable");
    }
    if (result == ESP_ERR_NOT_FOUND)
    {
        return local_web_send_error(request, "404 Not Found", not_found_error);
    }
    if (result == ESP_ERR_NOT_SUPPORTED)
    {
        return local_web_send_error(request, "400 Bad Request", "unsupported_operation");
    }
    return local_web_send_error(request, "500 Internal Server Error", "storage_io_failed");
}

static void local_web_publish_storage_status(uint8_t progress_percent,
                                             esp_err_t last_error)
{
    sd_card_manager_status_t sd_status = {0};
    sd_card_manager_filesystem_usage_t usage = {0};
    const bool status_ok = sd_card_manager_get_status(&sd_status) == ESP_OK;
    const bool available = status_ok &&
        (sd_status.state == SD_CARD_MANAGER_STATE_READY) &&
        (sd_card_manager_get_filesystem_usage(&usage) == ESP_OK);
    const ui_web_storage_status_t status = {
        .server_running = s_server != NULL,
        .storage_available = available,
        .progress_percent = progress_percent,
        .used_bytes = usage.used_bytes,
        .total_bytes = usage.total_bytes,
        .last_error = last_error,
    };
    (void)app_gui_post_web_storage_status(&status);
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
    static const httpd_uri_t storage_download = {
        .uri = "/api/storage/download", .method = HTTP_GET,
        .handler = local_web_storage_download_get,
    };
    static const httpd_uri_t storage_upload = {
        .uri = "/api/storage/upload", .method = HTTP_POST,
        .handler = local_web_storage_upload_post,
    };
    static const httpd_uri_t storage_delete = {
        .uri = "/api/storage/delete", .method = HTTP_POST,
        .handler = local_web_storage_delete_post,
    };
    static const httpd_uri_t storage_mkdir = {
        .uri = "/api/storage/mkdir", .method = HTTP_POST,
        .handler = local_web_storage_mkdir_post,
    };
    static const httpd_uri_t storage_rmdir = {
        .uri = "/api/storage/rmdir", .method = HTTP_POST,
        .handler = local_web_storage_rmdir_post,
    };
    static const httpd_uri_t storage_rename = {
        .uri = "/api/storage/rename", .method = HTTP_POST,
        .handler = local_web_storage_rename_post,
    };
    static const httpd_uri_t audio_status = {
        .uri = "/api/audio/status", .method = HTTP_GET,
        .handler = local_web_audio_status_get,
    };
    static const httpd_uri_t audio_tracks = {
        .uri = "/api/audio/tracks", .method = HTTP_GET,
        .handler = local_web_audio_tracks_get,
    };
    static const httpd_uri_t audio_play = {
        .uri = "/api/audio/play", .method = HTTP_POST,
        .handler = local_web_audio_play_post,
    };
    static const httpd_uri_t audio_control = {
        .uri = "/api/audio/control", .method = HTTP_POST,
        .handler = local_web_audio_control_post,
    };
    static const httpd_uri_t audio_volume = {
        .uri = "/api/audio/volume", .method = HTTP_POST,
        .handler = local_web_audio_volume_post,
    };

    esp_err_t result = httpd_register_uri_handler(server, &root);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_list);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_download);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_upload);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_delete);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_mkdir);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_rmdir);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_rename);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &audio_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &audio_tracks);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &audio_play);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &audio_control);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &audio_volume);
    return result;
}
