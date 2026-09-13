#include "xiaozhi_mcp_system_status.h"

#include <stdbool.h>
#include <stdio.h>

#include "app_log.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_foundation.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_system_status_query_provider_t s_provider = NULL;
static void *s_provider_context = NULL;
static bool s_attached = false;

static const char *const TAG = "XZ_SYSTEM_MCP";

/** Only composition-owned normalized identifier tokens may enter JSON text. */
static bool xiaozhi_mcp_system_status_is_identifier(
    const char *value,
    size_t value_size)
{
    if ((value == NULL) || (value_size == 0U)) {
        return false;
    }

    for (size_t index = 0U; index < value_size; ++index) {
        const char character = value[index];
        if (character == '\0') {
            return index > 0U;
        }
        if (!((character >= 'a') && (character <= 'z')) &&
            (character != '_')) {
            return false;
        }
    }
    return false;
}

static esp_err_t xiaozhi_mcp_system_status_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_foundation_system_status_query_provider_t provider = NULL;
    void *provider_context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_provider;
    provider_context = s_provider_context;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_foundation_system_status_query_snapshot_t snapshot = {0};
    const esp_err_t provider_ret =
        (provider == NULL) ? ESP_ERR_INVALID_STATE : provider(&snapshot, provider_context);
    const bool available = (provider_ret == ESP_OK) && snapshot.available &&
        xiaozhi_mcp_system_status_is_identifier(
            snapshot.overall_state, sizeof(snapshot.overall_state)) &&
        xiaozhi_mcp_system_status_is_identifier(
            snapshot.sensor_state, sizeof(snapshot.sensor_state)) &&
        xiaozhi_mcp_system_status_is_identifier(
            snapshot.cloud_state, sizeof(snapshot.cloud_state)) &&
        xiaozhi_mcp_system_status_is_identifier(
            snapshot.time_state, sizeof(snapshot.time_state)) &&
        xiaozhi_mcp_system_status_is_identifier(
            snapshot.storage_state, sizeof(snapshot.storage_state)) &&
        xiaozhi_mcp_system_status_is_identifier(
            snapshot.audio_state, sizeof(snapshot.audio_state));

    APP_LOGI(TAG, SYSTEM_STATUS_QUERY_CALLED_590A1E42,
             "Smart Room system-status MCP called; data=%s",
             available ? "available" : "unavailable");

    char text[400] = {0};
    char json[320] = {0};
    int text_written = 0;
    int json_written = 0;
    if (available) {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_SYSTEM_STATUS: overall_state=%s; sensor_state=%s; "
            "cloud_state=%s; time_state=%s; storage_state=%s; audio_state=%s. "
            "Report only these local states. This does not prove Wi-Fi or Internet reachability.",
            snapshot.overall_state,
            snapshot.sensor_state,
            snapshot.cloud_state,
            snapshot.time_state,
            snapshot.storage_state,
            snapshot.audio_state);
        json_written = snprintf(
            json, sizeof(json),
            "{\"data_available\":true,\"overall_state\":\"%s\","
            "\"sensor_state\":\"%s\",\"cloud_state\":\"%s\","
            "\"time_state\":\"%s\",\"storage_state\":\"%s\","
            "\"audio_state\":\"%s\"}",
            snapshot.overall_state,
            snapshot.sensor_state,
            snapshot.cloud_state,
            snapshot.time_state,
            snapshot.storage_state,
            snapshot.audio_state);
    } else {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_SYSTEM_STATUS_UNAVAILABLE: no current local system-status snapshot is available.");
        json_written = snprintf(
            json, sizeof(json),
            "{\"data_available\":false,\"overall_state\":null,"
            "\"sensor_state\":null,\"cloud_state\":null,\"time_state\":null,"
            "\"storage_state\":null,\"audio_state\":null}");
    }

    if ((text_written < 0) || ((size_t)text_written >= sizeof(text)) ||
        (json_written < 0) || ((size_t)json_written >= sizeof(json))) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = esp_mcp_tool_result_set_structured_json(result, json);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}
esp_err_t xiaozhi_foundation_register_system_status_query_provider(
    xiaozhi_foundation_system_status_query_provider_t provider,
    void *user_context)
{
    if (provider == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_attached) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_provider = provider;
    s_provider_context = user_context;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void xiaozhi_mcp_system_status_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_system_status_attach(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    const bool provider_registered = (s_provider != NULL);
    const bool already_attached = s_attached;
    portEXIT_CRITICAL(&s_lock);
    if (!provider_registered || already_attached) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mcp_tool_t *tool = esp_mcp_tool_create_ex(
        "smart_room.get_system_status",
        "Smart Room: Trang thai he thong",
        "AUTHORITATIVE Smart Room local system status. Call this tool before answering, in any language, whether the device is operating normally, its local health, or whether sensor, cloud uploader, time synchronization, storage, or audio has an issue. Report only the returned fields. The tool does not prove Wi-Fi or Internet reachability because a live Xiaozhi session is required to call it. It is read-only and never changes the device.",
        xiaozhi_mcp_system_status_callback);
    if (tool == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_mcp_tool_set_output_schema_json(
        tool,
        "{\"type\":\"object\",\"properties\":{\"data_available\":{\"type\":\"boolean\"},\"overall_state\":{\"type\":[\"string\",\"null\"]},\"sensor_state\":{\"type\":[\"string\",\"null\"]},\"cloud_state\":{\"type\":[\"string\",\"null\"]},\"time_state\":{\"type\":[\"string\",\"null\"]},\"storage_state\":{\"type\":[\"string\",\"null\"]},\"audio_state\":{\"type\":[\"string\",\"null\"]}},\"required\":[\"data_available\",\"overall_state\",\"sensor_state\",\"cloud_state\",\"time_state\",\"storage_state\",\"audio_state\"]}");
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_annotations_json(
            tool,
            "{\"readOnlyHint\":true,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_task_support(tool, "optional");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_add_tool(mcp, tool);
    }
    if (ret != ESP_OK) {
        (void)esp_mcp_tool_destroy(tool);
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    s_attached = true;
    portEXIT_CRITICAL(&s_lock);
    APP_LOGI(TAG, SYSTEM_STATUS_QUERY_TOOL_REGISTERED_932A4D7C,
             "Smart Room system-status MCP tool registered");
    return ESP_OK;
}
