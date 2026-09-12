#include "xiaozhi_mcp_light_set_state.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "cJSON.h"
#include "esp_mcp_property.h"
#include "esp_mcp_tool.h"
#include "freertos/FreeRTOS.h"
#include "xiaozhi_foundation.h"

#define XIAOZHI_MCP_LIGHT_MAX_POWER_BYTES  3U
#define XIAOZHI_MCP_LIGHT_MAX_COLOR_BYTES  7U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xiaozhi_foundation_light_set_state_provider_t s_provider = NULL;
static void *s_provider_context = NULL;
static bool s_attached = false;

#if CONFIG_XIAOZHI_FOUNDATION_LIGHT_SET_STATE_TOOL
static const char *const TAG = "XZ_LIGHT_MCP";

static bool xiaozhi_mcp_light_copy_named_color(
    const char *name,
    xiaozhi_foundation_light_set_state_request_t *request)
{
    if ((name == NULL) || (request == NULL) ||
        (strlen(name) > XIAOZHI_MCP_LIGHT_MAX_COLOR_BYTES)) {
        return false;
    }

    request->has_color = true;
    if (strcmp(name, "red") == 0) {
        request->red = 255U;
        request->green = 0U;
        request->blue = 0U;
    } else if (strcmp(name, "green") == 0) {
        request->red = 0U;
        request->green = 255U;
        request->blue = 0U;
    } else if (strcmp(name, "blue") == 0) {
        request->red = 0U;
        request->green = 0U;
        request->blue = 255U;
    } else if (strcmp(name, "white") == 0) {
        request->red = 255U;
        request->green = 255U;
        request->blue = 255U;
    } else if (strcmp(name, "yellow") == 0) {
        request->red = 255U;
        request->green = 255U;
        request->blue = 0U;
    } else if (strcmp(name, "cyan") == 0) {
        request->red = 0U;
        request->green = 255U;
        request->blue = 255U;
    } else if ((strcmp(name, "magenta") == 0) || (strcmp(name, "pink") == 0)) {
        request->red = 255U;
        request->green = 0U;
        request->blue = 255U;
    } else if (strcmp(name, "purple") == 0) {
        request->red = 128U;
        request->green = 0U;
        request->blue = 255U;
    } else if (strcmp(name, "orange") == 0) {
        request->red = 255U;
        request->green = 96U;
        request->blue = 0U;
    } else {
        request->has_color = false;
        return false;
    }

    return true;
}

static const char *xiaozhi_mcp_light_parse_request(
    const char *state_json,
    xiaozhi_foundation_light_set_state_request_t *request)
{
    if ((state_json == NULL) || (request == NULL)) {
        return "invalid_input";
    }

    cJSON *const state = cJSON_Parse(state_json);
    if ((state == NULL) || !cJSON_IsObject(state)) {
        cJSON_Delete(state);
        return "invalid_input";
    }

    *request = (xiaozhi_foundation_light_set_state_request_t){0};
    const char *error_code = NULL;
    for (const cJSON *field = state->child; field != NULL; field = field->next) {
        if ((field->string == NULL) || (strcmp(field->string, "power") == 0 && request->has_power) ||
            (strcmp(field->string, "color") == 0 && request->has_color) ||
            (strcmp(field->string, "brightness_percent") == 0 && request->has_brightness)) {
            error_code = "invalid_input";
            break;
        }

        if (strcmp(field->string, "power") == 0) {
            if (!cJSON_IsString(field) || (field->valuestring == NULL) ||
                (strlen(field->valuestring) > XIAOZHI_MCP_LIGHT_MAX_POWER_BYTES)) {
                error_code = "invalid_power";
                break;
            }
            if (strcmp(field->valuestring, "on") == 0) {
                request->power_on = true;
            } else if (strcmp(field->valuestring, "off") == 0) {
                request->power_on = false;
            } else {
                error_code = "invalid_power";
                break;
            }
            request->has_power = true;
        } else if (strcmp(field->string, "color") == 0) {
            if (!cJSON_IsString(field) || !xiaozhi_mcp_light_copy_named_color(field->valuestring, request)) {
                error_code = "unsupported_color";
                break;
            }
        } else if (strcmp(field->string, "brightness_percent") == 0) {
            if (!cJSON_IsNumber(field) || (field->valuedouble < 0.0) ||
                (field->valuedouble > 100.0) ||
                (field->valuedouble != (double)field->valueint)) {
                error_code = "brightness_out_of_range";
                break;
            }
            request->brightness_percent = (uint8_t)field->valueint;
            request->has_brightness = true;
        } else {
            error_code = "invalid_input";
            break;
        }
    }

    if ((error_code == NULL) && !request->has_power && !request->has_color &&
        !request->has_brightness) {
        error_code = "invalid_input";
    }
    if ((error_code == NULL) && request->has_power && !request->power_on &&
        (request->has_color || request->has_brightness)) {
        error_code = "invalid_field_combination";
    }

    cJSON_Delete(state);
    return error_code;
}

static esp_err_t xiaozhi_mcp_light_set_error(
    esp_mcp_tool_result_t *result,
    const char *error_code)
{
    char json[96] = {0};
    char text[128] = {0};
    const int json_written = snprintf(
        json, sizeof(json),
        "{\"success\":false,\"error_code\":\"%s\"}", error_code);
    const int text_written = snprintf(
        text, sizeof(text),
        "SMART_ROOM_LIGHT_SET_STATE_ERROR: %s.", error_code);
    if ((json_written < 0) || ((size_t)json_written >= sizeof(json)) ||
        (text_written < 0) || ((size_t)text_written >= sizeof(text))) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = esp_mcp_tool_result_set_is_error(result, true);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_set_structured_json(result, json);
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}

static const char *xiaozhi_mcp_light_outcome_to_error(
    xiaozhi_foundation_light_set_state_outcome_t outcome)
{
    switch (outcome) {
    case XIAOZHI_FOUNDATION_LIGHT_SET_STATE_MANAGER_NOT_INITIALIZED:
        return "manager_not_initialized";
    case XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SNAPSHOT_FAILED:
        return "snapshot_read_failed";
    case XIAOZHI_FOUNDATION_LIGHT_SET_STATE_APPLY_FAILED:
        return "manager_apply_failed";
    case XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SUCCESS:
    default:
        return "internal_error";
    }
}

static esp_err_t xiaozhi_mcp_light_set_state_callback(
    const esp_mcp_property_list_t *properties,
    esp_mcp_tool_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *const state_json = (properties == NULL) ? NULL :
        esp_mcp_property_list_get_property_object(properties, "state");
    xiaozhi_foundation_light_set_state_request_t request = {0};
    const char *const validation_error =
        xiaozhi_mcp_light_parse_request(state_json, &request);
    if (validation_error != NULL) {
        APP_LOGW(TAG, LIGHT_SET_STATE_REJECTED_4B099C1D,
                 "Smart Room light MCP rejected invalid request: %s", validation_error);
        return xiaozhi_mcp_light_set_error(result, validation_error);
    }

    xiaozhi_foundation_light_set_state_provider_t provider = NULL;
    void *provider_context = NULL;
    portENTER_CRITICAL(&s_lock);
    provider = s_provider;
    provider_context = s_provider_context;
    portEXIT_CRITICAL(&s_lock);
    if (provider == NULL) {
        return xiaozhi_mcp_light_set_error(result, "internal_provider_unavailable");
    }

    xiaozhi_foundation_light_set_state_result_t applied = {0};
    const esp_err_t provider_ret = provider(&request, &applied, provider_context);
    if ((provider_ret != ESP_OK) ||
        (applied.outcome != XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SUCCESS)) {
        const char *const error_code = (provider_ret == ESP_OK)
            ? xiaozhi_mcp_light_outcome_to_error(applied.outcome)
            : "internal_snapshot_failed";
        APP_LOGW(TAG, LIGHT_SET_STATE_FAILED_9E9387F0,
                 "Smart Room light MCP action failed: %s", error_code);
        return xiaozhi_mcp_light_set_error(result, error_code);
    }

    char json[144] = {0};
    char text[176] = {0};
    const int json_written = snprintf(
        json, sizeof(json),
        "{\"success\":true,\"power\":\"%s\",\"red\":%u,\"green\":%u,"
        "\"blue\":%u,\"brightness_percent\":%u}",
        applied.power_on ? "on" : "off",
        (unsigned)applied.red,
        (unsigned)applied.green,
        (unsigned)applied.blue,
        (unsigned)applied.brightness_percent);
    const int text_written = snprintf(
        text, sizeof(text),
        "SMART_ROOM_LIGHT_SET_STATE: success; power=%s; red=%u; green=%u; blue=%u; brightness_percent=%u.",
        applied.power_on ? "on" : "off",
        (unsigned)applied.red,
        (unsigned)applied.green,
        (unsigned)applied.blue,
        (unsigned)applied.brightness_percent);
    if ((json_written < 0) || ((size_t)json_written >= sizeof(json)) ||
        (text_written < 0) || ((size_t)text_written >= sizeof(text))) {
        return ESP_ERR_INVALID_SIZE;
    }

    APP_LOGI(TAG, LIGHT_SET_STATE_APPLIED_EC4E963A,
             "Smart Room light MCP applied logical state: power=%s brightness=%u",
             applied.power_on ? "on" : "off",
             (unsigned)applied.brightness_percent);
    esp_err_t ret = esp_mcp_tool_result_set_structured_json(result, json);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_result_add_text(result, text);
    }
    return ret;
}
#endif

esp_err_t xiaozhi_foundation_register_light_set_state_provider(
    xiaozhi_foundation_light_set_state_provider_t provider,
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

void xiaozhi_mcp_light_set_state_detach(void)
{
    portENTER_CRITICAL(&s_lock);
    s_attached = false;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xiaozhi_mcp_light_set_state_attach(esp_mcp_t *mcp)
{
    if (mcp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_XIAOZHI_FOUNDATION_LIGHT_SET_STATE_TOOL
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
        "light.set_state",
        "Smart Room: Dieu khien den",
        "Set the Smart Room NeoPixel logical state. Arguments must be {state:{...}}. Inside state, allow only power ('on' or 'off'), color (red, green, blue, white, yellow, cyan, magenta, pink, purple, or orange), and brightness_percent (integer 0..100). Provide at least one field. power='off' cannot be combined with color or brightness. Omitted fields preserve their current logical value. This is a controlled device action; report the returned state exactly.",
        xiaozhi_mcp_light_set_state_callback);
    if (tool == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_mcp_property_t *const state_property = esp_mcp_property_create(
        "state", ESP_MCP_PROPERTY_TYPE_OBJECT);
    if (state_property == NULL) {
        (void)esp_mcp_tool_destroy(tool);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_mcp_tool_add_property(tool, state_property);
    const bool property_added = (ret == ESP_OK);
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_output_schema_json(
            tool,
            "{\"type\":\"object\",\"properties\":{\"success\":{\"type\":\"boolean\"},\"error_code\":{\"type\":\"string\"},\"power\":{\"type\":\"string\",\"enum\":[\"on\",\"off\"]},\"red\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"green\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"blue\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"brightness_percent\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},\"required\":[\"success\"]}");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_annotations_json(
            tool,
            "{\"readOnlyHint\":false,\"destructiveHint\":false,\"idempotentHint\":true,\"openWorldHint\":false}");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_tool_set_task_support(tool, "optional");
    }
    if (ret == ESP_OK) {
        ret = esp_mcp_add_tool(mcp, tool);
    }
    if (ret != ESP_OK) {
        if (!property_added) {
            (void)esp_mcp_property_destroy(state_property);
        }
        (void)esp_mcp_tool_destroy(tool);
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    s_attached = true;
    portEXIT_CRITICAL(&s_lock);
    APP_LOGI(TAG, LIGHT_SET_STATE_TOOL_REGISTERED_1296E8CA,
             "Smart Room light.set_state MCP tool registered");
    return ESP_OK;
#endif
}
