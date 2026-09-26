#include "local_web_server.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"

#include "app_log.h"
#include "app_gui.h"
#include "audio_manager.h"
#include "cloud_manager.h"
#include "local_web_dashboard_policy.h"
#include "local_web_download.h"
#include "local_web_audio_policy.h"
#include "local_web_icon_policy.h"
#include "local_web_light_policy.h"
#include "local_web_path_policy.h"
#include "light_manager.h"
#include "log_manager.h"
#include "performance_monitor.h"
#include "sd_card_manager.h"
#include "scene_manager.h"
#include "sensor_manager.h"
#include "smart_room_mcp_adapter.h"
#include "voice_assistant_playback_control.h"
#include "wifi_manager.h"

#define LOCAL_WEB_HTTP_STACK_SIZE_BYTES 6144U
#define LOCAL_WEB_HTTP_MAX_OPEN_SOCKETS 2U
#define LOCAL_WEB_HTTP_ROUTE_COUNT 29U
#define LOCAL_WEB_HTTP_MAX_URI_LEN 1280U
#define LOCAL_WEB_RESPONSE_CHUNK_SIZE 512U
#define LOCAL_WEB_TRANSFER_CHUNK_SIZE SD_CARD_MANAGER_TRANSFER_CHUNK_SIZE
#define LOCAL_WEB_QUERY_VALUE_SIZE \
    ((LOCAL_WEB_LOGICAL_PATH_MAX_LEN * 3U) + 1U)
#define LOCAL_WEB_QUERY_BUFFER_SIZE \
    ((LOCAL_WEB_LOGICAL_PATH_MAX_LEN * 3U * 2U) + 24U)
#define LOCAL_WEB_LIGHT_QUERY_MAX_LEN 160U
#define LOCAL_WEB_SCENE_QUERY_MAX_LEN 32U

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
static esp_err_t local_web_icon_get(httpd_req_t *request);
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
static esp_err_t local_web_audio_seek_post(httpd_req_t *request);
static esp_err_t local_web_light_status_get(httpd_req_t *request);
static esp_err_t local_web_light_state_post(httpd_req_t *request);
static esp_err_t local_web_dashboard_status_get(httpd_req_t *request);
static esp_err_t local_web_scenes_get(httpd_req_t *request);
static esp_err_t local_web_scenes_status_get(httpd_req_t *request);
static esp_err_t local_web_scenes_apply_post(httpd_req_t *request);
static esp_err_t local_web_logs_status_get(httpd_req_t *request);
static esp_err_t local_web_logs_files_get(httpd_req_t *request);
static esp_err_t local_web_logs_read_get(httpd_req_t *request);
static esp_err_t local_web_diagnostics_status_get(httpd_req_t *request);
static esp_err_t local_web_diagnostics_export_get(httpd_req_t *request);
static esp_err_t local_web_send_error(
    httpd_req_t *request,
    const char *http_status,
    const char *error_code);
static esp_err_t local_web_send_json_escaped(
    httpd_req_t *request,
    const char *value);
static esp_err_t local_web_send_chunkf(httpd_req_t *request,
                                       const char *format, ...);
static esp_err_t local_web_send_responsef(httpd_req_t *request,
                                          const char *format, ...);
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
    const char *not_found_error,
    bool audio_catalog_changed);
static bool local_web_path_affects_audio_catalog(const char *logical_path);
static void local_web_publish_storage_status(uint8_t progress_percent,
                                             esp_err_t last_error);
static void local_web_publish_light_status(const light_manager_state_t *state,
                                           esp_err_t last_error);
static bool local_web_light_parse_state_query(httpd_req_t *request,
                                              local_web_light_update_t *update);
static esp_err_t local_web_light_send_state(httpd_req_t *request,
                                            const light_manager_state_t *state);
static esp_err_t local_web_light_send_manager_error(httpd_req_t *request,
                                                     esp_err_t result,
                                                     const char *failure_error);
static esp_err_t local_web_register_routes(httpd_handle_t server);
static bool local_web_scene_parse_apply_query(httpd_req_t *request,
                                              char scene_id[SCENE_MANAGER_ID_MAX_LEN + 1U]);
static esp_err_t local_web_scene_send_status(httpd_req_t *request,
                                             const scene_manager_status_t *status);

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
    /* Keep this in sync with local_web_register_routes(): HTTPD rejects the
     * whole startup when a new route has no registration slot. */
    config.max_uri_handlers = LOCAL_WEB_HTTP_ROUTE_COUNT;
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
    light_manager_state_t light_state = {0};
    const esp_err_t light_result = light_manager_get_state(&light_state);
    local_web_publish_light_status(
        light_result == ESP_OK ? &light_state : NULL,
        light_result);
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
        "\"sample_rate_hz\":%" PRIu32 ",\"position_granularity_frames\":%" PRIu32 ",\"volume_percent\":%" PRIu32 "}",
        local_web_audio_state_name(playback.state),
        local_web_audio_source_name(playback.source),
        playback.resumable ? "true" : "false", playback.generation,
        playback.position_frames, playback.total_frames,
        playback.sample_rate_hz, playback.position_granularity_frames, volume);
    if ((written < 0) || (written >= (int)sizeof(s_response_chunk))) {
        return local_web_send_error(request, "500 Internal Server Error", "response_too_large");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_light_send_state(httpd_req_t *request,
                                            const light_manager_state_t *state)
{
    const char *const effect =
        (state == NULL) ? NULL : local_web_light_effect_name(state->effect);
    if (effect == NULL) {
        return local_web_send_error(request, "500 Internal Server Error",
                                    "light_status_failed");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const int written = snprintf(
        s_response_chunk, sizeof(s_response_chunk),
        "{\"ok\":true,\"available\":true,\"power\":%s,"
        "\"red\":%u,\"green\":%u,\"blue\":%u,"
        "\"brightness_percent\":%u,\"effect\":\"%s\"}",
        state->power_on ? "true" : "false", (unsigned)state->red,
        (unsigned)state->green, (unsigned)state->blue,
        (unsigned)state->brightness_percent, effect);
    return ((written < 0) || (written >= (int)sizeof(s_response_chunk)))
               ? local_web_send_error(request, "500 Internal Server Error",
                                      "response_too_large")
               : httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_light_send_manager_error(httpd_req_t *request,
                                                     esp_err_t result,
                                                     const char *failure_error)
{
    switch (local_web_light_manager_result_from_error(result)) {
    case LOCAL_WEB_LIGHT_MANAGER_RESULT_UNAVAILABLE:
        return local_web_send_error(request, "503 Service Unavailable",
                                    "light_unavailable");
    case LOCAL_WEB_LIGHT_MANAGER_RESULT_BUSY:
        return local_web_send_error(request, "503 Service Unavailable", "light_busy");
    case LOCAL_WEB_LIGHT_MANAGER_RESULT_FAILED:
    case LOCAL_WEB_LIGHT_MANAGER_RESULT_OK:
    default:
        return local_web_send_error(request, "500 Internal Server Error", failure_error);
    }
}

static esp_err_t local_web_light_status_get(httpd_req_t *request)
{
    light_manager_state_t state = {0};
    const esp_err_t result = light_manager_get_state(&state);
    if (local_web_light_manager_result_from_error(result) ==
        LOCAL_WEB_LIGHT_MANAGER_RESULT_UNAVAILABLE) {
        local_web_publish_light_status(NULL, result);
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_sendstr(request, "{\"ok\":true,\"available\":false}");
    }
    if (result != ESP_OK) {
        local_web_publish_light_status(NULL, result);
        return local_web_light_send_manager_error(request, result, "light_status_failed");
    }
    local_web_publish_light_status(&state, ESP_OK);
    return local_web_light_send_state(request, &state);
}

static esp_err_t local_web_dashboard_status_get(httpd_req_t *request)
{
    sensor_manager_status_t sensor = {0};
    sd_card_manager_status_t storage = {0};
    sd_card_manager_filesystem_usage_t usage = {0};
    audio_manager_status_t audio = {0};
    audio_manager_playback_status_t playback = {0};
    light_manager_state_t light = {0};
    cloud_manager_status_t cloud = {0};
    wifi_manager_status_t network = {0};
    time_manager_status_t time = {0};

    const bool sensor_available = sensor_manager_get_status(&sensor) == ESP_OK;
    const bool storage_status_available = sd_card_manager_get_status(&storage) == ESP_OK;
    const bool capacity_valid = storage_status_available &&
        (storage.state == SD_CARD_MANAGER_STATE_READY) &&
        (sd_card_manager_get_filesystem_usage(&usage) == ESP_OK);
    const bool audio_available = audio_manager_get_status(&audio) == ESP_OK;
    const bool playback_available = audio_available &&
        (audio_manager_get_playback_status(&playback) == ESP_OK);
    const bool light_available = light_manager_get_state(&light) == ESP_OK;
    const bool cloud_available = cloud_manager_get_status(&cloud) == ESP_OK;
    const bool network_available = wifi_manager_get_status(&network) == ESP_OK;
    const bool time_available = time_manager_get_status(&time) == ESP_OK;
    const bool sensor_current = sensor_available &&
        local_web_dashboard_sensor_has_current_data(&sensor);
    const char *const light_effect = light_available
        ? local_web_light_effect_name(light.effect) : NULL;

    const local_web_dashboard_health_t health[] = {
        sensor_available ? local_web_dashboard_sensor_health(&sensor)
                         : LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
        storage_status_available
            ? local_web_dashboard_storage_health(&storage, capacity_valid)
            : LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
        audio_available ? local_web_dashboard_audio_health(&audio, playback_available)
                        : LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
        (light_available && (light_effect != NULL))
            ? LOCAL_WEB_DASHBOARD_HEALTH_NORMAL
            : LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
        cloud_available ? local_web_dashboard_cloud_health(&cloud)
                        : LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
        network_available ? local_web_dashboard_network_health(&network)
                          : LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
        time_available ? local_web_dashboard_time_health(&time)
                       : LOCAL_WEB_DASHBOARD_HEALTH_UNAVAILABLE,
    };
    const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000U;
    const bool last_success_known = sensor_available &&
        (sensor.last_success_time_ms > 0) &&
        ((uint64_t)sensor.last_success_time_ms <= now_ms);

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = local_web_send_chunkf(
        request, "{\"ok\":true,\"overall_state\":\"%s\",\"sensor\":{"
        "\"available\":%s,\"state\":\"%s\",\"data_valid\":%s,"
        "\"data_stale\":%s,\"temperature_c\":",
        local_web_dashboard_overall_state(health, sizeof(health) / sizeof(health[0])),
        sensor_available ? "true" : "false",
        sensor_available ? local_web_dashboard_sensor_state_name(sensor.state) : "unavailable",
        sensor_current ? "true" : "false",
        (sensor_available && sensor.data_stale) || !sensor_current ? "true" : "false");
    if (result != ESP_OK) return result;
    result = sensor_current
        ? local_web_send_chunkf(request, "%.2f,\"humidity_percent\":%.2f",
                                (double)sensor.temperature_c, (double)sensor.humidity_percent)
        : httpd_resp_sendstr_chunk(request, "null,\"humidity_percent\":null");
    if (result != ESP_OK) return result;
    result = last_success_known
        ? local_web_send_chunkf(request, ",\"last_success_age_ms\":%" PRIu64 "}",
                                local_web_dashboard_age_ms(now_ms,
                                                           sensor.last_success_time_ms))
        : httpd_resp_sendstr_chunk(request, ",\"last_success_age_ms\":null}");
    if (result != ESP_OK) return result;

    result = local_web_send_chunkf(
        request, ",\"storage\":{\"available\":%s,\"state\":\"%s\","
        "\"capacity_valid\":%s,\"total_bytes\":",
        capacity_valid ? "true" : "false",
        storage_status_available
            ? local_web_dashboard_storage_state_name(storage.state) : "unavailable",
        capacity_valid ? "true" : "false");
    if (result != ESP_OK) return result;
    result = capacity_valid
        ? local_web_send_chunkf(request, "%" PRIu64 ",\"used_bytes\":%" PRIu64
                                  ",\"free_bytes\":%" PRIu64,
                                  usage.total_bytes, usage.used_bytes, usage.free_bytes)
        : httpd_resp_sendstr_chunk(request,
                                   "null,\"used_bytes\":null,\"free_bytes\":null");
    if (result != ESP_OK) return result;
    result = storage_status_available
        ? local_web_send_chunkf(request, ",\"active_leases\":%" PRIu32 "}",
                                storage.active_leases)
        : httpd_resp_sendstr_chunk(request, ",\"active_leases\":null}");
    if (result != ESP_OK) return result;

    result = local_web_send_chunkf(
        request, ",\"audio\":{\"available\":%s,\"state\":\"%s\","
        "\"playback_available\":%s,\"playback_state\":\"%s\","
        "\"playback_source\":\"%s\",\"volume_percent\":%" PRIu32 "}",
        audio_available ? "true" : "false",
        audio_available ? local_web_dashboard_audio_state_name(audio.state) : "unavailable",
        playback_available ? "true" : "false",
        playback_available ? local_web_audio_state_name(playback.state) : "unavailable",
        playback_available ? local_web_audio_source_name(playback.source) : "unavailable",
        audio_available ? audio.playback_volume_percent : 0U);
    if (result != ESP_OK) return result;

    result = local_web_send_chunkf(
        request, ",\"light\":{\"available\":%s,\"power\":%s,\"red\":%u,"
        "\"green\":%u,\"blue\":%u,\"brightness_percent\":%u,\"effect\":\"%s\"}",
        (light_available && (light_effect != NULL)) ? "true" : "false",
        light_available && light.power_on ? "true" : "false",
        light_available ? (unsigned)light.red : 0U,
        light_available ? (unsigned)light.green : 0U,
        light_available ? (unsigned)light.blue : 0U,
        light_available ? (unsigned)light.brightness_percent : 0U,
        (light_effect != NULL) ? light_effect : "unavailable");
    if (result != ESP_OK) return result;

    result = local_web_send_chunkf(
        request, ",\"cloud\":{\"available\":%s,\"state\":\"%s\"}",
        cloud_available ? "true" : "false",
        cloud_available ? local_web_dashboard_cloud_state_name(cloud.state) : "unavailable");
    if (result != ESP_OK) return result;

    result = local_web_send_chunkf(
        request, ",\"network\":{\"available\":%s,\"state\":\"%s\","
        "\"has_ipv4_address\":%s,\"ipv4_address\":\"",
        network_available ? "true" : "false",
        network_available ? local_web_dashboard_network_state_name(network.state)
                          : "unavailable",
        network_available && network.has_ipv4_address ? "true" : "false");
    if (result != ESP_OK) return result;
    if (network_available && network.has_ipv4_address) {
        result = local_web_send_json_escaped(request, network.ipv4_address);
        if (result != ESP_OK) return result;
    }
    result = local_web_send_chunkf(request, "\",\"rssi_valid\":%s,\"rssi_dbm\":",
                                   network_available && network.rssi_valid ? "true" : "false");
    if (result != ESP_OK) return result;
    result = network_available && network.rssi_valid
        ? local_web_send_chunkf(request, "%d}", (int)network.rssi_dbm)
        : httpd_resp_sendstr_chunk(request, "null}");
    if (result != ESP_OK) return result;

    result = local_web_send_chunkf(
        request, ",\"time\":{\"available\":%s,\"state\":\"%s\",\"synced\":%s},"
        "\"system\":{\"uptime_ms\":%" PRIu64 "}}",
        time_available ? "true" : "false",
        time_available ? local_web_dashboard_time_state_name(time.state) : "unavailable",
        time_available && time.synced ? "true" : "false", now_ms);
    return result == ESP_OK ? httpd_resp_send_chunk(request, NULL, 0U) : result;
}

static bool local_web_light_parse_state_query(httpd_req_t *request,
                                              local_web_light_update_t *update)
{
    if ((request == NULL) || (update == NULL) ||
        (httpd_req_get_url_query_len(request) == 0U) ||
        (httpd_req_get_url_query_len(request) >= LOCAL_WEB_LIGHT_QUERY_MAX_LEN) ||
        (httpd_req_get_url_query_len(request) >= sizeof(s_query_buffer)) ||
        (httpd_req_get_url_query_str(request, s_query_buffer,
                                     sizeof(s_query_buffer)) != ESP_OK)) {
        return false;
    }
    char *field = s_query_buffer;
    while (field[0] != '\0') {
        char *const separator = strchr(field, '&');
        if (separator != NULL) *separator = '\0';
        char *const equals = strchr(field, '=');
        if ((equals == NULL) || (equals == field) || (equals[1] == '\0')) {
            return false;
        }
        *equals = '\0';
        if (!local_web_light_update_set_field(update, field, equals + 1U)) {
            return false;
        }
        if (separator == NULL) break;
        field = separator + 1U;
        if (field[0] == '\0') return false;
    }
    return local_web_light_update_is_valid(update);
}

static esp_err_t local_web_light_state_post(httpd_req_t *request)
{
    local_web_light_update_t update = {0};
    if (!local_web_light_parse_state_query(request, &update)) {
        return local_web_send_error(request, "400 Bad Request", "invalid_light_request");
    }

    light_manager_state_t requested = {0};
    esp_err_t result = light_manager_get_state(&requested);
    if (result != ESP_OK) {
        local_web_publish_light_status(NULL, result);
        return local_web_light_send_manager_error(request, result, "light_status_failed");
    }
    if (!local_web_light_apply_update(&update, &requested)) {
        return local_web_send_error(request, "400 Bad Request", "invalid_light_request");
    }

    result = light_manager_set_state(&requested);
    if (result != ESP_OK) {
        if (local_web_light_manager_result_from_error(result) ==
            LOCAL_WEB_LIGHT_MANAGER_RESULT_UNAVAILABLE) {
            local_web_publish_light_status(NULL, result);
        }
        return local_web_light_send_manager_error(request, result, "light_apply_failed");
    }

    light_manager_state_t confirmed = {0};
    result = light_manager_get_state(&confirmed);
    if (result != ESP_OK) {
        local_web_publish_light_status(NULL, result);
        return local_web_light_send_manager_error(request, result, "light_status_failed");
    }
    local_web_publish_light_status(&confirmed, ESP_OK);
    return local_web_light_send_state(request, &confirmed);
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

static bool local_web_scene_parse_apply_query(
    httpd_req_t *request,
    char scene_id[SCENE_MANAGER_ID_MAX_LEN + 1U])
{
    if ((request == NULL) || (scene_id == NULL) ||
        (httpd_req_get_url_query_len(request) == 0U) ||
        (httpd_req_get_url_query_len(request) >= LOCAL_WEB_SCENE_QUERY_MAX_LEN) ||
        (httpd_req_get_url_query_len(request) >= sizeof(s_query_buffer)) ||
        (httpd_req_get_url_query_str(request, s_query_buffer,
                                     sizeof(s_query_buffer)) != ESP_OK)) {
        return false;
    }

    bool id_seen = false;
    char *field = s_query_buffer;
    while (field[0] != '\0') {
        char *const separator = strchr(field, '&');
        if (separator != NULL) *separator = '\0';
        char *const equals = strchr(field, '=');
        if ((equals == NULL) || (equals == field) || (equals[1] == '\0')) return false;
        *equals = '\0';
        if ((strcmp(field, "id") != 0) || id_seen ||
            (strlen(equals + 1U) > SCENE_MANAGER_ID_MAX_LEN)) return false;
        (void)strncpy(scene_id, equals + 1U, SCENE_MANAGER_ID_MAX_LEN);
        scene_id[SCENE_MANAGER_ID_MAX_LEN] = '\0';
        id_seen = true;
        if (separator == NULL) break;
        field = separator + 1U;
    }
    return id_seen;
}

static esp_err_t local_web_scene_send_status(
    httpd_req_t *request,
    const scene_manager_status_t *status)
{
    if ((request == NULL) || (status == NULL)) return ESP_ERR_INVALID_ARG;
    const int written = snprintf(
        s_response_chunk, sizeof(s_response_chunk),
        "{\"ok\":true,\"available\":true,\"generation\":%" PRIu32 ","
        "\"last_requested_scene\":\"%s\",\"last_completed_scene\":\"%s\","
        "\"outcome\":\"%s\",\"light\":{\"outcome\":\"%s\"},"
        "\"audio\":{\"outcome\":\"%s\"}}",
        status->generation, status->last_requested_scene,
        status->last_completed_scene,
        scene_manager_outcome_to_string(status->outcome),
        scene_manager_outcome_to_string(status->light_outcome),
        scene_manager_outcome_to_string(status->audio_outcome));
    if ((written < 0) || (written >= (int)sizeof(s_response_chunk))) {
        return local_web_send_error(request, "500 Internal Server Error", "response_too_large");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, s_response_chunk, written);
}

static esp_err_t local_web_scenes_get(httpd_req_t *request)
{
    scene_manager_catalog_t catalog = {0};
    if (scene_manager_get_catalog(&catalog) != ESP_OK) {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_sendstr(request, "{\"ok\":true,\"available\":false}");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (httpd_resp_sendstr_chunk(request, "{\"ok\":true,\"scenes\":[") != ESP_OK) return ESP_FAIL;
    for (uint8_t index = 0U; index < catalog.count; ++index) {
        const int written = snprintf(s_response_chunk, sizeof(s_response_chunk),
                                     "%s{\"id\":\"%s\",\"name\":\"%s\"}",
                                     (index == 0U) ? "" : ",",
                                     catalog.entries[index].id,
                                     catalog.entries[index].name);
        if ((written < 0) || (written >= (int)sizeof(s_response_chunk)) ||
            (httpd_resp_send_chunk(request, s_response_chunk, written) != ESP_OK)) return ESP_FAIL;
    }
    return httpd_resp_sendstr_chunk(request, "]}") == ESP_OK
               ? httpd_resp_send_chunk(request, NULL, 0U) : ESP_FAIL;
}

static esp_err_t local_web_scenes_status_get(httpd_req_t *request)
{
    scene_manager_status_t status = {0};
    const esp_err_t result = scene_manager_get_status(&status);
    if (result == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_sendstr(request, "{\"ok\":true,\"available\":false}");
    }
    if (result == ESP_ERR_TIMEOUT) return local_web_send_error(request, "409 Conflict", "scene_busy");
    if (result != ESP_OK) return local_web_send_error(request, "500 Internal Server Error", "scene_status_failed");
    return local_web_scene_send_status(request, &status);
}

static esp_err_t local_web_scenes_apply_post(httpd_req_t *request)
{
    char scene_id[SCENE_MANAGER_ID_MAX_LEN + 1U] = {0};
    if (!local_web_scene_parse_apply_query(request, scene_id)) {
        return local_web_send_error(request, "400 Bad Request", "invalid_scene_request");
    }
    scene_manager_status_t status = {0};
    const esp_err_t result = scene_manager_apply(scene_id, &status);
    if (result == ESP_ERR_INVALID_ARG) return local_web_send_error(request, "400 Bad Request", "unknown_scene");
    if (result == ESP_ERR_INVALID_STATE) return local_web_send_error(request, "503 Service Unavailable", "scene_unavailable");
    if (result != ESP_OK) return local_web_send_error(request, "500 Internal Server Error", "scene_apply_failed");
    if ((status.outcome == SCENE_MANAGER_OUTCOME_BUSY) ||
        (status.outcome == SCENE_MANAGER_OUTCOME_REJECTED)) {
        return local_web_send_error(request, "409 Conflict", "scene_busy");
    }
    if (status.outcome == SCENE_MANAGER_OUTCOME_UNAVAILABLE) {
        return local_web_send_error(request, "503 Service Unavailable", "scene_unavailable");
    }
    return local_web_scene_send_status(request, &status);
}

static esp_err_t local_web_logs_status_get(httpd_req_t *request)
{
    log_manager_stats_t stats = {0};
    const esp_err_t result = log_manager_get_stats(&stats);
    if (result == ESP_ERR_INVALID_STATE) return local_web_send_error(request, "503 Service Unavailable", "logs_unavailable");
    if (result == ESP_ERR_TIMEOUT) return local_web_send_error(request, "409 Conflict", "logs_busy");
    if (result != ESP_OK) return local_web_send_error(request, "500 Internal Server Error", "logs_status_failed");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return local_web_send_responsef(request,
        "{\"ok\":true,\"available\":true,\"storage_available\":%s,"
        "\"time_synchronized\":%s,\"produced_records\":%" PRIu64 ","
        "\"persisted_records\":%" PRIu64 ",\"durable_records\":%" PRIu64 ","
        "\"dropped_records\":%" PRIu64 ",\"storage_write_failures\":%" PRIu32 ","
        "\"file_rotations\":%" PRIu32 ",\"buffered_bytes\":%u,"
        "\"peak_buffered_bytes\":%u,\"buffer_capacity\":%u}",
        stats.storage_available ? "true" : "false", stats.time_synchronized ? "true" : "false",
        stats.produced_records, stats.persisted_records, stats.durable_records,
        stats.dropped_records, stats.storage_write_failures, stats.file_rotations,
        (unsigned)stats.buffered_bytes, (unsigned)stats.peak_buffered_bytes,
        (unsigned)stats.buffer_capacity);
}

static esp_err_t local_web_logs_files_get(httpd_req_t *request)
{
    log_manager_archive_list_t archives = {0};
    const esp_err_t result = log_manager_list_archives(&archives);
    if (result == ESP_ERR_INVALID_STATE) return local_web_send_error(request, "503 Service Unavailable", "logs_unavailable");
    if (result == ESP_ERR_TIMEOUT) return local_web_send_error(request, "409 Conflict", "logs_busy");
    if (result != ESP_OK) return local_web_send_error(request, "500 Internal Server Error", "logs_list_failed");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (httpd_resp_sendstr_chunk(request, "{\"ok\":true,\"available\":true,\"files\":[") != ESP_OK) return ESP_FAIL;
    for (uint8_t index = 0U; index < archives.count; ++index) {
        if (local_web_send_chunkf(request,
            "%s{\"id\":\"%s\",\"size_bytes\":%" PRIu64 ",\"time_named\":%s}",
            index == 0U ? "" : ",", archives.archives[index].id,
            archives.archives[index].size_bytes,
            archives.archives[index].time_named ? "true" : "false") != ESP_OK) return ESP_FAIL;
    }
    return local_web_send_chunkf(request, "],\"truncated\":%s}",
                                 archives.truncated ? "true" : "false") == ESP_OK
        ? httpd_resp_send_chunk(request, NULL, 0U) : ESP_FAIL;
}

static bool local_web_logs_read_query(httpd_req_t *request, char *id, size_t id_size,
                                      uint64_t *offset)
{
    if ((request == NULL) || (id == NULL) || (offset == NULL) ||
        httpd_req_get_url_query_len(request) == 0U ||
        httpd_req_get_url_query_len(request) >= 64U ||
        httpd_req_get_url_query_str(request, s_query_buffer, sizeof(s_query_buffer)) != ESP_OK) return false;
    bool got_id = false, got_offset = false;
    char *field = s_query_buffer;
    while (*field) {
        char *separator = strchr(field, '&');
        if (separator) *separator = '\0';
        char *equals = strchr(field, '=');
        if (!equals || equals == field || !equals[1]) return false;
        *equals = '\0';
        if (!strcmp(field, "id") && !got_id && strlen(equals + 1U) < id_size) {
            (void)snprintf(id, id_size, "%s", equals + 1U); got_id = true;
        } else if (!strcmp(field, "offset") && !got_offset &&
                   local_web_audio_uint64_parse(equals + 1U, offset)) {
            got_offset = true;
        } else return false;
        if (!separator) break;
        field = separator + 1U;
    }
    return got_id && got_offset;
}

static esp_err_t local_web_logs_read_get(httpd_req_t *request)
{
    char id[LOG_MANAGER_ARCHIVE_ID_LEN + 1U] = {0};
    uint64_t offset = 0U;
    if (!local_web_logs_read_query(request, id, sizeof(id), &offset))
        return local_web_send_error(request, "400 Bad Request", "invalid_log_read_request");
    log_manager_archive_page_t page = {0};
    const esp_err_t result = log_manager_read_archive(id, offset, &page);
    if (result == ESP_ERR_NOT_FOUND) return local_web_send_error(request, "404 Not Found", "log_not_found");
    if (result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_SIZE)
        return local_web_send_error(request, "400 Bad Request", "invalid_log_cursor");
    if (result == ESP_ERR_INVALID_STATE) return local_web_send_error(request, "503 Service Unavailable", "logs_unavailable");
    if (result == ESP_ERR_TIMEOUT) return local_web_send_error(request, "409 Conflict", "logs_busy");
    if (result != ESP_OK) return local_web_send_error(request, "500 Internal Server Error", "logs_read_failed");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (local_web_send_chunkf(request, "{\"ok\":true,\"id\":\"%s\",\"offset\":%" PRIu64 ","
        "\"next_offset\":%" PRIu64 ",\"next_offset_valid\":%s,\"eof\":%s,\"malformed_record_count\":%u,"
        "\"details_omitted\":true,\"records\":[", id, page.offset, page.next_offset,
        page.next_offset_valid ? "true" : "false", page.eof ? "true" : "false",
        (unsigned)page.malformed_record_count) != ESP_OK) return ESP_FAIL;
    for (uint8_t index = 0U; index < page.record_count; ++index) {
        const log_manager_public_record_t *record = &page.records[index];
        const esp_err_t send_result = record->time_valid
            ? local_web_send_chunkf(request,
                "%s{\"timestamp\":\"%s\",\"time_valid\":true,\"uptime_ms\":%" PRIu64 ","
                "\"level\":\"%s\",\"tag\":\"%s\",\"event\":\"%s\"}",
                index == 0U ? "" : ",", record->timestamp, record->uptime_ms,
                record->level, record->tag, record->event)
            : local_web_send_chunkf(request,
                "%s{\"timestamp\":null,\"time_valid\":false,\"uptime_ms\":%" PRIu64 ","
                "\"level\":\"%s\",\"tag\":\"%s\",\"event\":\"%s\"}",
                index == 0U ? "" : ",", record->uptime_ms,
                record->level, record->tag, record->event);
        if (send_result != ESP_OK) return ESP_FAIL;
    }
    return httpd_resp_sendstr_chunk(request, "]}") == ESP_OK
        ? httpd_resp_send_chunk(request, NULL, 0U) : ESP_FAIL;
}

static esp_err_t local_web_diagnostics_status_get(httpd_req_t *request)
{
    performance_monitor_status_t performance = {0};
    log_manager_stats_t logging = {0};
    const bool performance_available = performance_monitor_get_status(&performance) == ESP_OK;
    const bool logging_available = log_manager_get_stats(&logging) == ESP_OK;
    const int64_t now_us = esp_timer_get_time();
    const bool age_valid = performance_available && performance.sample_valid &&
        now_us >= 0 && performance.captured_at_us >= 0 &&
        performance.captured_at_us <= now_us;
    char age_ms[24] = "null";
    if (age_valid) {
        (void)snprintf(age_ms, sizeof(age_ms), "%llu",
                       (unsigned long long)((now_us - performance.captured_at_us) / 1000));
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return local_web_send_responsef(request,
        "{\"ok\":true,\"performance\":{\"available\":%s,\"sample_valid\":%s,"
        "\"report_index\":%" PRIu32 ",\"age_ms\":%s,\"cpu\":{\"used_x10\":%" PRIu32 ","
        "\"peak_500ms_x10\":%" PRIu32 ",\"idle_x10\":%" PRIu32 "}},"
        "\"logging\":{\"available\":%s,\"produced_records\":%" PRIu64 ","
        "\"durable_records\":%" PRIu64 ",\"dropped_records\":%" PRIu64 "}}",
        performance_available ? "true" : "false", performance.sample_valid ? "true" : "false",
        performance.report_index, age_ms, performance.cpu_used_x10,
        performance.cpu_peak_500ms_x10, performance.cpu_idle_x10,
        logging_available ? "true" : "false", logging.produced_records,
        logging.durable_records, logging.dropped_records);
}

static esp_err_t local_web_diagnostics_export_get(httpd_req_t *request)
{
    performance_monitor_status_t performance = {0};
    log_manager_stats_t logging = {0};
    const bool performance_available = performance_monitor_get_status(&performance) == ESP_OK;
    const bool logging_available = log_manager_get_stats(&logging) == ESP_OK;
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return local_web_send_responsef(request,
        "Smart Room Diagnostic Report\nCPU\navailable=%u\nsample_valid=%u\nused_x10=%" PRIu32
        "\npeak_500ms_x10=%" PRIu32 "\nLOGGING\navailable=%u\nproduced=%" PRIu64
        "\ndurable=%" PRIu64 "\ndropped=%" PRIu64 "\n",
        performance_available, performance.sample_valid, performance.cpu_used_x10,
        performance.cpu_peak_500ms_x10, logging_available, logging.produced_records,
        logging.durable_records, logging.dropped_records);
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

static esp_err_t local_web_audio_seek_post(httpd_req_t *request)
{
    if (!local_web_get_query_value(request, "frames")) {
        return local_web_send_error(request, "400 Bad Request", "invalid_target");
    }
    uint64_t target_frames = 0U;
    if (!local_web_audio_uint64_parse(s_query_value, &target_frames)) {
        return local_web_send_error(request, "400 Bad Request", "invalid_target");
    }
    uint64_t generation = 0U;
    if (!local_web_get_query_value(request, "generation") ||
        !local_web_audio_uint64_parse(s_query_value, &generation)) {
        return local_web_send_error(request, "400 Bad Request", "invalid_generation");
    }
    if ((generation == 0U) || (generation > UINT32_MAX)) {
        return local_web_send_error(request, "400 Bad Request", "invalid_generation");
    }
    voice_assistant_playback_control_result_t result = {0};
    if (voice_assistant_playback_seek((uint32_t)generation,
                                      target_frames, &result) != ESP_OK) {
        return local_web_send_error(request, "500 Internal Server Error", "internal_error");
    }
    if (!result.accepted) {
        const char *error =
            (result.playback.generation != (uint32_t)generation) ? "invalid_state" :
            ((result.playback.total_frames > 0U) &&
             (target_frames >= result.playback.total_frames)) ? "invalid_target" :
            ((result.playback.position_granularity_frames > 0U) &&
             ((target_frames % result.playback.position_granularity_frames) != 0U))
                ? "invalid_target" :
            ((result.outcome == VOICE_ASSISTANT_PLAYBACK_OUTCOME_NON_RESUMABLE_SOURCE) ||
             (result.outcome == VOICE_ASSISTANT_PLAYBACK_OUTCOME_NO_CURRENT_SOURCE))
                ? "playback_not_seekable" : "invalid_state";
        return local_web_send_error(request, "409 Conflict", error);
    }
    return httpd_resp_sendstr(request, "{\"ok\":true,\"accepted\":true}");
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
        return local_web_send_storage_result(
            request, begin_result, "file_not_found", false);
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

static esp_err_t local_web_icon_get(httpd_req_t *request)
{
    if (!local_web_get_query_value(request, "name"))
    {
        return local_web_send_error(request, "400 Bad Request", "invalid_icon");
    }

    const char *const logical_path =
        local_web_icon_logical_path(s_query_value);
    if (logical_path == NULL)
    {
        return local_web_send_error(request, "404 Not Found", "icon_not_found");
    }

    sd_card_manager_transfer_info_t transfer = {0};
    const esp_err_t begin_result =
        sd_card_manager_download_begin(logical_path, &transfer);
    if (begin_result != ESP_OK)
    {
        return local_web_send_storage_result(
            request, begin_result, "icon_not_found", false);
    }

    httpd_resp_set_type(request, "image/svg+xml");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = ESP_OK;
    for (;;)
    {
        size_t read_size = 0U;
        result = sd_card_manager_download_read(
            transfer.transfer_id, s_transfer_chunk, sizeof(s_transfer_chunk),
            &read_size);
        if ((result != ESP_OK) || (read_size == 0U))
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
        return local_web_send_storage_result(
            request, begin_result, "upload_failed", false);
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
        return local_web_send_storage_result(
            request, result, "upload_failed", false);
    }
    if (local_web_path_affects_audio_catalog(logical_path))
    {
        smart_room_mcp_adapter_audio_catalog_invalidate();
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
    return local_web_send_storage_result(
        request, sd_card_manager_delete_file(logical_path), "file_not_found",
        local_web_path_affects_audio_catalog(logical_path));
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
    return local_web_send_storage_result(
        request, sd_card_manager_make_directory(logical_path), "parent_not_found",
        local_web_path_affects_audio_catalog(logical_path));
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
    return local_web_send_storage_result(
        request, sd_card_manager_remove_empty_directory(logical_path), "directory_not_found",
        local_web_path_affects_audio_catalog(logical_path));
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
    return local_web_send_storage_result(
        request, sd_card_manager_rename_path(source, destination), "source_not_found",
        local_web_path_affects_audio_catalog(source) ||
            local_web_path_affects_audio_catalog(destination));
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
    const char *not_found_error,
    bool audio_catalog_changed)
{
    if (result == ESP_OK)
    {
        if (audio_catalog_changed)
        {
            smart_room_mcp_adapter_audio_catalog_invalidate();
        }
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

static bool local_web_path_affects_audio_catalog(const char *logical_path)
{
    static const char audio_root[] = "/audio";
    const size_t root_length = sizeof(audio_root) - 1U;
    return (logical_path != NULL) &&
           (strncmp(logical_path, audio_root, root_length) == 0) &&
           ((logical_path[root_length] == '\0') ||
            (logical_path[root_length] == '/'));
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

static ui_web_light_effect_t local_web_light_effect_to_ui(
    light_manager_effect_t effect)
{
    switch (effect) {
        case LIGHT_MANAGER_EFFECT_SOLID: return UI_WEB_LIGHT_EFFECT_SOLID;
        case LIGHT_MANAGER_EFFECT_BLINK: return UI_WEB_LIGHT_EFFECT_BLINK;
        case LIGHT_MANAGER_EFFECT_BREATH: return UI_WEB_LIGHT_EFFECT_BREATH;
        case LIGHT_MANAGER_EFFECT_PULSE: return UI_WEB_LIGHT_EFFECT_PULSE;
        case LIGHT_MANAGER_EFFECT_RAINBOW: return UI_WEB_LIGHT_EFFECT_RAINBOW;
        case LIGHT_MANAGER_EFFECT_STROBE: return UI_WEB_LIGHT_EFFECT_STROBE;
        case LIGHT_MANAGER_EFFECT_HEARTBEAT: return UI_WEB_LIGHT_EFFECT_HEARTBEAT;
        case LIGHT_MANAGER_EFFECT_CANDLE: return UI_WEB_LIGHT_EFFECT_CANDLE;
        case LIGHT_MANAGER_EFFECT_SOS: return UI_WEB_LIGHT_EFFECT_SOS;
        case LIGHT_MANAGER_EFFECT_LIGHTNING: return UI_WEB_LIGHT_EFFECT_LIGHTNING;
        case LIGHT_MANAGER_EFFECT_WAKE_UP: return UI_WEB_LIGHT_EFFECT_WAKE_UP;
        case LIGHT_MANAGER_EFFECT_SLEEP_FADE: return UI_WEB_LIGHT_EFFECT_SLEEP_FADE;
        case LIGHT_MANAGER_EFFECT_NOTIFICATION: return UI_WEB_LIGHT_EFFECT_NOTIFICATION;
        default: return UI_WEB_LIGHT_EFFECT_UNKNOWN;
    }
}

static void local_web_publish_light_status(const light_manager_state_t *state,
                                           esp_err_t last_error)
{
    const ui_web_light_status_t status = {
        .server_running = s_server != NULL,
        .light_available = (state != NULL) && (last_error == ESP_OK),
        .power_on = state != NULL ? state->power_on : false,
        .red = state != NULL ? state->red : 0U,
        .green = state != NULL ? state->green : 0U,
        .blue = state != NULL ? state->blue : 0U,
        .brightness_percent = state != NULL ? state->brightness_percent : 0U,
        .effect = state != NULL
                      ? local_web_light_effect_to_ui(state->effect)
                      : UI_WEB_LIGHT_EFFECT_UNKNOWN,
        .last_error = last_error,
    };
    (void)app_gui_post_web_light_status(&status);
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

static esp_err_t local_web_send_chunkf(httpd_req_t *request,
                                       const char *format, ...)
{
    if ((request == NULL) || (format == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(s_response_chunk, sizeof(s_response_chunk),
                                  format, arguments);
    va_end(arguments);
    return ((written < 0) || (written >= (int)sizeof(s_response_chunk)))
               ? ESP_ERR_INVALID_SIZE
               : httpd_resp_send_chunk(request, s_response_chunk, written);
}

/* A complete, bounded response must not leave HTTPD in chunked mode. */
static esp_err_t local_web_send_responsef(httpd_req_t *request,
                                          const char *format, ...)
{
    if ((request == NULL) || (format == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(s_response_chunk, sizeof(s_response_chunk),
                                  format, arguments);
    va_end(arguments);
    return ((written < 0) || (written >= (int)sizeof(s_response_chunk)))
               ? ESP_ERR_INVALID_SIZE
               : httpd_resp_send(request, s_response_chunk, written);
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
    static const httpd_uri_t icon = {
        .uri = "/api/assets/icon", .method = HTTP_GET,
        .handler = local_web_icon_get,
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
    static const httpd_uri_t audio_seek = {
        .uri = "/api/audio/seek", .method = HTTP_POST,
        .handler = local_web_audio_seek_post,
    };
    static const httpd_uri_t light_status = {
        .uri = "/api/light/status", .method = HTTP_GET,
        .handler = local_web_light_status_get,
    };
    static const httpd_uri_t light_state = {
        .uri = "/api/light/state", .method = HTTP_POST,
        .handler = local_web_light_state_post,
    };
    static const httpd_uri_t dashboard_status = {
        .uri = "/api/dashboard/status", .method = HTTP_GET,
        .handler = local_web_dashboard_status_get,
    };
    static const httpd_uri_t scenes = {
        .uri = "/api/scenes", .method = HTTP_GET, .handler = local_web_scenes_get,
    };
    static const httpd_uri_t scenes_status = {
        .uri = "/api/scenes/status", .method = HTTP_GET,
        .handler = local_web_scenes_status_get,
    };
    static const httpd_uri_t scenes_apply = {
        .uri = "/api/scenes/apply", .method = HTTP_POST,
        .handler = local_web_scenes_apply_post,
    };
    static const httpd_uri_t logs_status = {
        .uri = "/api/logs/status", .method = HTTP_GET,
        .handler = local_web_logs_status_get,
    };
    static const httpd_uri_t logs_files = {
        .uri = "/api/logs/files", .method = HTTP_GET,
        .handler = local_web_logs_files_get,
    };
    static const httpd_uri_t logs_read = {
        .uri = "/api/logs/read", .method = HTTP_GET,
        .handler = local_web_logs_read_get,
    };
    static const httpd_uri_t diagnostics_status = { .uri = "/api/diagnostics/status", .method = HTTP_GET, .handler = local_web_diagnostics_status_get };
    static const httpd_uri_t diagnostics_export = { .uri = "/api/diagnostics/export", .method = HTTP_GET, .handler = local_web_diagnostics_export_get };

    esp_err_t result = httpd_register_uri_handler(server, &root);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_list);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &storage_download);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &icon);
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
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &audio_seek);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &light_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &light_state);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &dashboard_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &scenes);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &scenes_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &scenes_apply);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &logs_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &logs_files);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &logs_read);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &diagnostics_status);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &diagnostics_export);
    return result;
}
