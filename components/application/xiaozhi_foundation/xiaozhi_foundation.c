/**
 * @file xiaozhi_foundation.c
 * @brief Isolated, non-sensitive Xiaozhi service-information probe.
 *
 * This component copies only scalar availability state from the Xiaozhi
 * service response. It neither exposes Xiaozhi-owned string pointers nor
 * takes ownership of Wi-Fi or Xiaozhi lifecycle management.
 */

/* Includes ----------------------------------------------------------------- */
#include "xiaozhi_foundation.h"

#include <string.h>

#include "esp_log.h"
#include "esp_xiaozhi_info.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define XIAOZHI_FOUNDATION_PROBE_TASK_NAME \
    "xiaozhi_probe"

#define XIAOZHI_FOUNDATION_PROBE_TASK_STACK_SIZE \
    (8U * 1024U)

#define XIAOZHI_FOUNDATION_PROBE_TASK_PRIORITY \
    4U

static portMUX_TYPE s_probe_lock =
    portMUX_INITIALIZER_UNLOCKED;

static bool s_probe_in_progress = false;

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "XIAOZHI_FOUNDATION";

/* Functions ---------------------------------------------------------------- */
esp_err_t xiaozhi_foundation_probe(xiaozhi_foundation_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_info, 0, sizeof(*out_info));

    esp_xiaozhi_chat_info_t info = {0};

    ESP_LOGI(TAG, "Probing Xiaozhi service");

    esp_err_t ret = esp_xiaozhi_chat_get_info(&info);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Xiaozhi service probe failed: %s",
                 esp_err_to_name(ret));

        /* get_info() may have partially allocated fields. */
        (void)esp_xiaozhi_chat_free_info(&info);

        return ret;
    }

    /*
     * Copy only scalar/non-sensitive information.
     * Do not expose Xiaozhi-owned string pointers.
     */
    out_info->service_reachable = true;

    out_info->mqtt_available =
        info.has_mqtt_config;

    out_info->websocket_available =
        info.has_websocket_config;

    out_info->activation_code_available =
        info.has_activation_code;

    out_info->activation_challenge_available =
        info.has_activation_challenge;

    out_info->activation_timeout_ms =
        info.activation_timeout_ms;

    out_info->server_time_available =
        info.has_server_time;

    out_info->new_firmware_available =
        info.has_new_version;

    ESP_LOGI(TAG, "Service reachable");
    ESP_LOGI(TAG, "MQTT available: %s",
             out_info->mqtt_available ? "yes" : "no");
    ESP_LOGI(TAG, "WebSocket available: %s",
             out_info->websocket_available ? "yes" : "no");
    ESP_LOGI(TAG, "Activation code: %s",
             out_info->activation_code_available ? "present" : "none");
    ESP_LOGI(TAG, "Activation challenge: %s",
             out_info->activation_challenge_available ? "present" : "none");
    ESP_LOGI(TAG, "Activation timeout: %d ms",
             out_info->activation_timeout_ms);

    ret = esp_xiaozhi_chat_free_info(&info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Failed to free Xiaozhi info: %s",
                 esp_err_to_name(ret));

        return ret;
    }

    return ESP_OK;
}

static void xiaozhi_foundation_probe_task(void *argument)
{
    (void)argument;

    xiaozhi_foundation_info_t info = {0};

    const esp_err_t ret =
        xiaozhi_foundation_probe(&info);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Background service probe failed: %s",
            esp_err_to_name(ret));
    } else {
        ESP_LOGI(
            TAG,
            "Background service probe completed");
    }

    portENTER_CRITICAL(&s_probe_lock);

    s_probe_in_progress = false;

    portEXIT_CRITICAL(&s_probe_lock);

    vTaskDelete(NULL);
}

esp_err_t xiaozhi_foundation_request_probe(void)
{
    portENTER_CRITICAL(&s_probe_lock);

    if (s_probe_in_progress) {
        portEXIT_CRITICAL(&s_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_probe_in_progress = true;

    portEXIT_CRITICAL(&s_probe_lock);

    const BaseType_t task_created =
        xTaskCreate(
            xiaozhi_foundation_probe_task,
            XIAOZHI_FOUNDATION_PROBE_TASK_NAME,
            XIAOZHI_FOUNDATION_PROBE_TASK_STACK_SIZE,
            NULL,
            XIAOZHI_FOUNDATION_PROBE_TASK_PRIORITY,
            NULL);

    if (task_created != pdPASS) {
        portENTER_CRITICAL(&s_probe_lock);

        s_probe_in_progress = false;

        portEXIT_CRITICAL(&s_probe_lock);

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
