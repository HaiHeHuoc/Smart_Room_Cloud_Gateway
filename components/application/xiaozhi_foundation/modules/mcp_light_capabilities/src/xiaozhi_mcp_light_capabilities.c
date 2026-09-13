#include "xiaozhi_mcp_light_capabilities.h"

#include <stdbool.h>

#include "app_log.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_attached = false;

static const char *const TAG = "XZ_LIGHT_CAPS";

static esp_err_t xiaozhi_mcp_light_capabilities_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    (void)properties;
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static const char json[] =
        "{\"features\":[\"power\",\"color\",\"brightness\",\"effect\"],"
        "\"colors\":[\"red\",\"green\",\"blue\",\"white\",\"yellow\",\"cyan\","
        "\"magenta\",\"pink\",\"purple\",\"orange\"],"
        "\"effects\":[\"solid\",\"blink\",\"breath\",\"pulse\",\"rainbow\"],"
        "\"brightness_percent\":{\"min\":0,\"max\":100}}";
    static const char text[] =
        "SMART_ROOM_LIGHT_CAPABILITIES: features=power,color,brightness,effect; "
        "colors=red,green,blue,white,yellow,cyan,magenta,pink,purple,orange; "
        "effects=solid,blink,breath,pulse,rainbow; brightness_percent=0..100. "
        "Use only these values when controlling the light.";

    APP_LOGI(TAG, LIGHT_CAPABILITIES_CALLED_8EA65E1B,
             "Smart Room light capabilities MCP called");
    esp_err_t ret = esp_mcp_tool_result_set_structured_json(result, json);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}
void xiaozhi_mcp_light_capabilities_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_light_capabilities_attach(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    const bool already_attached = s_attached;
    portEXIT_CRITICAL(&s_lock);
    if (already_attached) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mcp_tool_t *tool = esp_mcp_tool_create_ex(
        "light.get_capabilities",
        "Smart Room: Tinh nang den ho tro",
        "AUTHORITATIVE Smart Room light capabilities. ALWAYS call this tool before answering any user question, in any language, about which light features, colors, effects, or brightness range the device supports. Report only returned values. This tool is read-only and never changes the device.",
        xiaozhi_mcp_light_capabilities_callback);
    if (tool == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_mcp_tool_set_output_schema_json(
        tool,
        "{\"type\":\"object\",\"properties\":{\"features\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"colors\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"effects\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"brightness_percent\":{\"type\":\"object\",\"properties\":{\"min\":{\"type\":\"integer\"},\"max\":{\"type\":\"integer\"}},\"required\":[\"min\",\"max\"]}},\"required\":[\"features\",\"colors\",\"effects\",\"brightness_percent\"]}");
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
    APP_LOGI(TAG, LIGHT_CAPABILITIES_TOOL_REGISTERED_0371D0C6,
             "Smart Room light.get_capabilities MCP tool registered");
    return ESP_OK;
}
