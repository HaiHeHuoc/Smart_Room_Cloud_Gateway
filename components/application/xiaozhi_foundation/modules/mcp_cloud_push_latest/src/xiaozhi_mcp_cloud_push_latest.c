#include "xiaozhi_mcp_cloud_push_latest.h"
#include "xiaozhi_mcp_cloud_push_latest_policy.h"

#include <stdbool.h>
#include <stdio.h>

#include "app_log.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_foundation.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_cloud_push_latest_provider_t s_provider = NULL;
static void *s_provider_context = NULL;
static bool s_attached = false;

static const char *const TAG = "XZ_CLOUD_PUSH";

static esp_err_t xiaozhi_mcp_cloud_push_latest_set_result(
    esp_mcp_tool_result_t *result,
    xiaozhi_foundation_cloud_push_latest_outcome_t outcome,
    bool accepted)
{
    const char *const outcome_name =
        xiaozhi_mcp_cloud_push_latest_outcome_name(outcome);
    const bool is_accepted = accepted &&
        xiaozhi_mcp_cloud_push_latest_is_accepted(outcome);
    char json[160] = {0};
    char text[240] = {0};
    const int json_written = snprintf(
        json, sizeof(json),
        "{\"accepted\":%s,\"upload_complete\":false,\"result\":\"%s\"}",
        is_accepted ? "true" : "false",
        outcome_name);
    int text_written = 0;
    if (is_accepted) {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_CLOUD_PUSH_LATEST: accepted; upload_complete=false. The cloud worker will attempt the latest product-owned snapshot later; this is not Firebase upload success.");
    } else {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_CLOUD_PUSH_LATEST_ERROR: %s; upload_complete=false. No Firebase upload success is claimed.",
            outcome_name);
    }
    if ((json_written < 0) || ((size_t)json_written >= sizeof(json)) ||
        (text_written < 0) || ((size_t)text_written >= sizeof(text))) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = ESP_OK;
    if (!is_accepted) {
        ret = esp_mcp_tool_result_set_is_error(result, true);
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_set_structured_json(result, json);
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}

static esp_err_t xiaozhi_mcp_cloud_push_latest_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_foundation_cloud_push_latest_provider_t provider = NULL;
    void *provider_context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_provider;
    provider_context = s_provider_context;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_foundation_cloud_push_latest_result_t provider_result = {
        .outcome = XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_FAILED,
        .accepted = false,
    };
    const esp_err_t provider_ret = (provider == NULL)
        ? ESP_ERR_INVALID_STATE
        : provider(&provider_result, provider_context);
    if (provider_ret != ESP_OK) {
        provider_result.outcome = XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_FAILED;
        provider_result.accepted = false;
    }

    const bool accepted = provider_result.accepted &&
        xiaozhi_mcp_cloud_push_latest_is_accepted(provider_result.outcome);
    APP_LOGI(TAG, CLOUD_PUSH_LATEST_CALLED_3C81AA26,
             "Smart Room cloud push-latest MCP result=%s",
             xiaozhi_mcp_cloud_push_latest_outcome_name(provider_result.outcome));
    return xiaozhi_mcp_cloud_push_latest_set_result(
        result, provider_result.outcome, accepted);
}

esp_err_t xiaozhi_foundation_register_cloud_push_latest_provider(
    xiaozhi_foundation_cloud_push_latest_provider_t provider,
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

void xiaozhi_mcp_cloud_push_latest_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_cloud_push_latest_attach(esp_mcp_t *mcp)
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
        "cloud.push_latest",
        "Smart Room: Gui trang thai moi nhat len cloud",
        "Request one bounded cloud upload of the latest Smart Room telemetry snapshot. This tool takes no arguments and cannot choose telemetry values, Firebase paths, URLs, credentials, tokens, or HTTP behavior. Report accepted only as scheduling acceptance: it never proves Firebase upload completion. Use smart_room.get_cloud_sync_status later for uploader state.",
        xiaozhi_mcp_cloud_push_latest_callback);
    if (tool == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_mcp_tool_set_output_schema_json(
        tool,
        "{\"type\":\"object\",\"properties\":{\"accepted\":{\"type\":\"boolean\"},\"upload_complete\":{\"type\":\"boolean\",\"const\":false},\"result\":{\"type\":\"string\",\"enum\":[\"accepted\",\"not_ready\",\"offline\",\"busy\",\"invalid_state\",\"failed\"]}},\"required\":[\"accepted\",\"upload_complete\",\"result\"]}");
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_annotations_json(
            tool,
            "{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":false,\"openWorldHint\":false}");
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
    APP_LOGI(TAG, CLOUD_PUSH_LATEST_TOOL_REGISTERED_284A3420,
             "Smart Room cloud.push_latest MCP tool registered");
    return ESP_OK;
}
