#include "xiaozhi_mcp_cloud_sync.h"

#include <stdbool.h>
#include <stdio.h>

#include "app_log.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_foundation.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_cloud_sync_query_provider_t s_provider = NULL;
static void *s_provider_context = NULL;
static bool s_attached = false;

#if CONFIG_XIAOZHI_FOUNDATION_CLOUD_SYNC_QUERY_TOOL
static const char *const TAG = "XZ_CLOUD_MCP";

/** Only provider-owned normalized identifier tokens may enter JSON text. */
static bool xiaozhi_mcp_cloud_sync_is_identifier(
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

static esp_err_t xiaozhi_mcp_cloud_sync_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_foundation_cloud_sync_query_provider_t provider = NULL;
    void *provider_context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_provider;
    provider_context = s_provider_context;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_foundation_cloud_sync_query_snapshot_t snapshot = {0};
    const esp_err_t provider_ret =
        (provider == NULL) ? ESP_ERR_INVALID_STATE : provider(&snapshot, provider_context);
    const bool available = (provider_ret == ESP_OK) && snapshot.available &&
        xiaozhi_mcp_cloud_sync_is_identifier(
            snapshot.state, sizeof(snapshot.state)) &&
        xiaozhi_mcp_cloud_sync_is_identifier(
            snapshot.failure_class, sizeof(snapshot.failure_class));

    APP_LOGI(TAG, CLOUD_SYNC_QUERY_CALLED_9255D1EA,
             "Smart Room cloud-sync MCP called; data=%s",
             available ? "available" : "unavailable");

    char text[320] = {0};
    char json[320] = {0};
    int text_written = 0;
    int json_written = 0;
    if (available) {
        char last_success_age_json[16] = {0};
        char retry_delay_json[16] = {0};
        const char *const last_success_age_text = snapshot.last_success_available
            ? last_success_age_json : "unavailable";
        const char *const retry_delay_text = snapshot.retry_scheduled
            ? retry_delay_json : "unavailable";
        const int last_success_age_written = snprintf(
            last_success_age_json, sizeof(last_success_age_json), "%lu",
            (unsigned long)snapshot.last_success_age_seconds);
        const int retry_delay_written = snprintf(
            retry_delay_json, sizeof(retry_delay_json), "%lu",
            (unsigned long)snapshot.retry_delay_seconds);
        if ((last_success_age_written < 0) ||
            ((size_t)last_success_age_written >= sizeof(last_success_age_json)) ||
            (retry_delay_written < 0) ||
            ((size_t)retry_delay_written >= sizeof(retry_delay_json))) {
            return ESP_ERR_INVALID_SIZE;
        }

        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_CLOUD_SYNC_STATUS: cloud_state=%s; "
            "last_success_age_seconds=%s; consecutive_failure_count=%lu; "
            "failure_class=%s; retry_scheduled=%s; retry_delay_seconds=%s. "
            "This reports uploader state only; do not infer Internet reachability or "
            "claim that a newer sensor sample has already uploaded.",
            snapshot.state,
            last_success_age_text,
            (unsigned long)snapshot.consecutive_failure_count,
            snapshot.failure_class,
            snapshot.retry_scheduled ? "true" : "false",
            retry_delay_text);
        json_written = snprintf(
            json, sizeof(json),
            "{\"data_available\":true,\"cloud_state\":\"%s\","
            "\"last_success_available\":%s,\"last_success_age_seconds\":%s,"
            "\"consecutive_failure_count\":%lu,\"failure_class\":\"%s\","
            "\"retry_scheduled\":%s,\"retry_delay_seconds\":%s}",
            snapshot.state,
            snapshot.last_success_available ? "true" : "false",
            snapshot.last_success_available ? last_success_age_json : "null",
            (unsigned long)snapshot.consecutive_failure_count,
            snapshot.failure_class,
            snapshot.retry_scheduled ? "true" : "false",
            snapshot.retry_scheduled ? retry_delay_json : "null");
    } else {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_CLOUD_SYNC_UNAVAILABLE: no current cloud uploader status is available.");
        json_written = snprintf(
            json, sizeof(json),
            "{\"data_available\":false,\"cloud_state\":null,"
            "\"last_success_available\":false,\"last_success_age_seconds\":null,"
            "\"consecutive_failure_count\":null,\"failure_class\":null,"
            "\"retry_scheduled\":false,\"retry_delay_seconds\":null}");
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
#endif

esp_err_t xiaozhi_foundation_register_cloud_sync_query_provider(
    xiaozhi_foundation_cloud_sync_query_provider_t provider,
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

void xiaozhi_mcp_cloud_sync_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_cloud_sync_attach(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_XIAOZHI_FOUNDATION_CLOUD_SYNC_QUERY_TOOL
    return ESP_OK;
#else
    portENTER_CRITICAL(&s_lock);
    const bool provider_registered = (s_provider != NULL);
    const bool already_attached = s_attached;
    portEXIT_CRITICAL(&s_lock);
    if (!provider_registered || already_attached) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mcp_tool_t *tool = esp_mcp_tool_create_ex(
        "smart_room.get_cloud_sync_status",
        "Smart Room: Trang thai dong bo cloud",
        "AUTHORITATIVE Smart Room cloud uploader status. Call this tool before answering, in any language, whether Smart Room data has synchronized to cloud or Firebase, when the latest upload succeeded, whether uploads are retrying, or whether cloud synchronization has failed. Report only the returned uploader state and fields. This tool does not prove Internet reachability and does not prove that a newer sensor sample has uploaded. It is read-only and never changes the device.",
        xiaozhi_mcp_cloud_sync_callback);
    if (tool == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_mcp_tool_set_output_schema_json(
        tool,
        "{\"type\":\"object\",\"properties\":{\"data_available\":{\"type\":\"boolean\"},\"cloud_state\":{\"type\":[\"string\",\"null\"]},\"last_success_available\":{\"type\":\"boolean\"},\"last_success_age_seconds\":{\"type\":[\"integer\",\"null\"],\"minimum\":0},\"consecutive_failure_count\":{\"type\":[\"integer\",\"null\"],\"minimum\":0},\"failure_class\":{\"type\":[\"string\",\"null\"]},\"retry_scheduled\":{\"type\":\"boolean\"},\"retry_delay_seconds\":{\"type\":[\"integer\",\"null\"],\"minimum\":0}},\"required\":[\"data_available\",\"cloud_state\",\"last_success_available\",\"last_success_age_seconds\",\"consecutive_failure_count\",\"failure_class\",\"retry_scheduled\",\"retry_delay_seconds\"]}");
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
    APP_LOGI(TAG, CLOUD_SYNC_QUERY_TOOL_REGISTERED_4D9021B3,
             "Smart Room cloud-sync MCP tool registered");
    return ESP_OK;
#endif
}
