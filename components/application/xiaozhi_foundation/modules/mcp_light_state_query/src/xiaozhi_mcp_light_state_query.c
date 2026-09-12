#include "xiaozhi_mcp_light_state_query.h"

#include <stdbool.h>
#include <stdio.h>

#include "app_log.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_foundation.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_light_state_query_provider_t s_provider = NULL;
static void *s_provider_context = NULL;
static bool s_attached = false;

#if CONFIG_XIAOZHI_FOUNDATION_LIGHT_STATE_QUERY_TOOL
static const char *const TAG = "XZ_LIGHT_QUERY";

static const char *xiaozhi_mcp_light_state_color_name(
    const xiaozhi_foundation_light_state_query_snapshot_t *snapshot)
{
    if ((snapshot->red == 255U) && (snapshot->green == 0U) && (snapshot->blue == 0U)) {
        return "red";
    }
    if ((snapshot->red == 0U) && (snapshot->green == 255U) && (snapshot->blue == 0U)) {
        return "green";
    }
    if ((snapshot->red == 0U) && (snapshot->green == 0U) && (snapshot->blue == 255U)) {
        return "blue";
    }
    if ((snapshot->red == 255U) && (snapshot->green == 255U) && (snapshot->blue == 255U)) {
        return "white";
    }
    if ((snapshot->red == 255U) && (snapshot->green == 255U) && (snapshot->blue == 0U)) {
        return "yellow";
    }
    if ((snapshot->red == 0U) && (snapshot->green == 255U) && (snapshot->blue == 255U)) {
        return "cyan";
    }
    if ((snapshot->red == 255U) && (snapshot->green == 0U) && (snapshot->blue == 255U)) {
        return "pink";
    }
    if ((snapshot->red == 128U) && (snapshot->green == 0U) && (snapshot->blue == 255U)) {
        return "purple";
    }
    if ((snapshot->red == 255U) && (snapshot->green == 96U) && (snapshot->blue == 0U)) {
        return "orange";
    }
    return "custom";
}

static const char *xiaozhi_mcp_light_state_effect_name(
    xiaozhi_foundation_light_effect_t effect)
{
    switch (effect) {
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_SOLID: return "solid";
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_BLINK: return "blink";
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_BREATH: return "breath";
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_PULSE: return "pulse";
    case XIAOZHI_FOUNDATION_LIGHT_EFFECT_RAINBOW: return "rainbow";
    default: return NULL;
    }
}

static esp_err_t xiaozhi_mcp_light_state_query_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xiaozhi_foundation_light_state_query_provider_t provider = NULL;
    void *provider_context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_provider;
    provider_context = s_provider_context;
    portEXIT_CRITICAL(&s_lock);

    xiaozhi_foundation_light_state_query_snapshot_t snapshot = {0};
    const esp_err_t provider_ret =
        (provider == NULL) ? ESP_ERR_INVALID_STATE : provider(&snapshot, provider_context);
    const bool available = (provider_ret == ESP_OK) && snapshot.available;

    char text[240] = {0};
    char json[224] = {0};
    int text_written = 0;
    int json_written = 0;
    if (available) {
        const char *const color_name = xiaozhi_mcp_light_state_color_name(&snapshot);
        const char *const effect_name = xiaozhi_mcp_light_state_effect_name(snapshot.effect);
        if (effect_name == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_LIGHT_STATE: power=%s; color=%s; red=%u; green=%u; blue=%u; brightness_percent=%u; effect=%s. Use these exact values to answer the user.",
            snapshot.power_on ? "on" : "off",
            color_name,
            (unsigned)snapshot.red,
            (unsigned)snapshot.green,
            (unsigned)snapshot.blue,
            (unsigned)snapshot.brightness_percent,
            effect_name);
        json_written = snprintf(
            json, sizeof(json),
            "{\"state_available\":true,\"power\":\"%s\",\"color\":\"%s\",\"red\":%u,\"green\":%u,\"blue\":%u,\"brightness_percent\":%u,\"effect\":\"%s\"}",
            snapshot.power_on ? "on" : "off",
            color_name,
            (unsigned)snapshot.red,
            (unsigned)snapshot.green,
            (unsigned)snapshot.blue,
            (unsigned)snapshot.brightness_percent,
            effect_name);
    } else {
        text_written = snprintf(
            text, sizeof(text),
            "SMART_ROOM_LIGHT_STATE_UNAVAILABLE: no current logical light state is available.");
        json_written = snprintf(
            json, sizeof(json),
            "{\"state_available\":false,\"power\":null,\"color\":null,\"red\":null,\"green\":null,\"blue\":null,\"brightness_percent\":null,\"effect\":null}");
    }

    if ((text_written < 0) || ((size_t)text_written >= sizeof(text)) ||
        (json_written < 0) || ((size_t)json_written >= sizeof(json))) {
        return ESP_ERR_INVALID_SIZE;
    }

    APP_LOGI(TAG, LIGHT_STATE_QUERY_CALLED_C199E187,
             "Smart Room light-state MCP called; state=%s",
             available ? "available" : "unavailable");
    esp_err_t ret = esp_mcp_tool_result_set_structured_json(result, json);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}
#endif

esp_err_t xiaozhi_foundation_register_light_state_query_provider(
    xiaozhi_foundation_light_state_query_provider_t provider,
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

void xiaozhi_mcp_light_state_query_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_light_state_query_attach(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_XIAOZHI_FOUNDATION_LIGHT_STATE_QUERY_TOOL
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
        "light.get_state",
        "Smart Room: Trang thai den hien tai",
        "AUTHORITATIVE Smart Room logical light state. ALWAYS call this tool before answering any user question, in any language, about whether the light is on or off, its current brightness, color, RGB values, or active product effect. Report only the returned fields. The tool is read-only and never changes the device.",
        xiaozhi_mcp_light_state_query_callback);
    if (tool == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_mcp_tool_set_output_schema_json(
        tool,
        "{\"type\":\"object\",\"properties\":{\"state_available\":{\"type\":\"boolean\"},\"power\":{\"type\":[\"string\",\"null\"],\"enum\":[\"on\",\"off\",null]},\"color\":{\"type\":[\"string\",\"null\"]},\"red\":{\"type\":[\"integer\",\"null\"],\"minimum\":0,\"maximum\":255},\"green\":{\"type\":[\"integer\",\"null\"],\"minimum\":0,\"maximum\":255},\"blue\":{\"type\":[\"integer\",\"null\"],\"minimum\":0,\"maximum\":255},\"brightness_percent\":{\"type\":[\"integer\",\"null\"],\"minimum\":0,\"maximum\":100},\"effect\":{\"type\":[\"string\",\"null\"],\"enum\":[\"solid\",\"blink\",\"breath\",\"pulse\",\"rainbow\",null]}},\"required\":[\"state_available\",\"power\",\"color\",\"red\",\"green\",\"blue\",\"brightness_percent\",\"effect\"]}");
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
    APP_LOGI(TAG, LIGHT_STATE_QUERY_TOOL_REGISTERED_9F9E3A8D,
             "Smart Room light.get_state MCP tool registered");
    return ESP_OK;
#endif
}
