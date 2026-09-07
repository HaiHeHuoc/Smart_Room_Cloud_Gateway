#pragma once
#include "esp_log.h"
#define APP_LOGI(tag, event, ...) ESP_LOGI(tag, __VA_ARGS__)
#define APP_LOGW(tag, event, ...) ESP_LOGW(tag, __VA_ARGS__)
#define APP_LOGE(tag, event, ...) ESP_LOGW(tag, __VA_ARGS__)
#define APP_LOGD(tag, event, ...) ESP_LOGI(tag, __VA_ARGS__)
#define APP_LOGV(tag, event, ...) ESP_LOGI(tag, __VA_ARGS__)
