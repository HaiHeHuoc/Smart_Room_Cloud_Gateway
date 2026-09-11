#include "xiaozhi_mcp_sensor_query.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "app_log.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_foundation.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_sensor_query_provider_t s_provider = NULL;
static void *s_provider_context = NULL;
static bool s_attached = false;

#if CONFIG_XIAOZHI_FOUNDATION_SENSOR_QUERY_TOOL
static const char *const TAG = "XZ_SENSOR_MCP";

static esp_err_t xiaozhi_mcp_sensor_query_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_foundation_sensor_query_provider_t provider = NULL;
    void *provider_context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_provider;
    provider_context = s_provider_context;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_foundation_sensor_query_snapshot_t snapshot = {0};
    const esp_err_t provider_ret =
        (provider == NULL) ? ESP_ERR_INVALID_STATE : provider(&snapshot, provider_context);
    const bool available =
        (provider_ret == ESP_OK) && snapshot.available &&
        isfinite(snapshot.temperature_c) && isfinite(snapshot.humidity_percent);

    APP_LOGI(TAG, SENSOR_QUERY_CALLED_8F1B7D4C,
             "Smart Room sensor MCP called; data=%s",
             available ? "available" : "unavailable");

    char text[160] = {0};
    char json[160] = {0};
    int text_written = 0;
    int json_written = 0;
    if (available) {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_SENSOR_READING: temperature_c=%.1f; humidity_percent=%.1f. Use these exact values to answer the user.",
            (double)snapshot.temperature_c,
            (double)snapshot.humidity_percent);
        json_written = snprintf(
            json, sizeof(json),
            "{\"data_available\":true,\"temperature_c\":%.1f,\"humidity_percent\":%.1f}",
            (double)snapshot.temperature_c,
            (double)snapshot.humidity_percent);
    } else {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_SENSOR_UNAVAILABLE: no current valid temperature and humidity sample is available.");
        json_written = snprintf(
            json, sizeof(json),
            "{\"data_available\":false,\"temperature_c\":null,\"humidity_percent\":null}");
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

esp_err_t xiaozhi_foundation_register_sensor_query_provider(
    xiaozhi_foundation_sensor_query_provider_t provider,
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

void xiaozhi_mcp_sensor_query_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_sensor_query_attach(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_XIAOZHI_FOUNDATION_SENSOR_QUERY_TOOL
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
        "smart_room.get_current_temperature_humidity",
        "Smart Room: Nhiet do va do am hien tai",
        "AUTHORITATIVE Smart Room sensor data. ALWAYS call this tool before answering any user question, in any language, about the room temperature, room humidity, climate, or current Smart Room conditions. Return the temperature_c and humidity_percent values exactly. This tool is read-only and never changes the device.",
        xiaozhi_mcp_sensor_query_callback);
    if (tool == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_mcp_tool_set_output_schema_json(
        tool,
        "{\"type\":\"object\",\"properties\":{\"data_available\":{\"type\":\"boolean\"},\"temperature_c\":{\"type\":[\"number\",\"null\"],\"description\":\"Current room temperature in degrees Celsius\"},\"humidity_percent\":{\"type\":[\"number\",\"null\"],\"description\":\"Current room relative humidity in percent\"}},\"required\":[\"data_available\",\"temperature_c\",\"humidity_percent\"]}");
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
    APP_LOGI(TAG, SENSOR_QUERY_TOOL_REGISTERED_2E0AFB88,
             "Smart Room temperature/humidity MCP tool registered");
    return ESP_OK;
#endif
}
