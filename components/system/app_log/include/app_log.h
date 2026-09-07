#pragma once

/** @file app_log.h
 * Task-context producer interface. No filesystem I/O or per-record allocation.
 * Not ISR/cache-disabled/panic safe. Console may block like normal ESP-IDF logs;
 * persistence never waits for the ring mutex. Never pass secrets as arguments.
 * Tags/events should be short static identifiers; details should use key=value.
 */
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif
void app_log_emit(esp_log_level_t level, const char *tag, const char *event,
                  const char *format, ...) __attribute__((format(printf, 4, 5)));
#ifdef __cplusplus
}
#endif

/* Event tokens are stringified; no enum/registration is needed. */
#define APP_LOGE(tag, event, ...) app_log_emit(ESP_LOG_ERROR, tag, #event, __VA_ARGS__)
#define APP_LOGW(tag, event, ...) app_log_emit(ESP_LOG_WARN, tag, #event, __VA_ARGS__)
#define APP_LOGI(tag, event, ...) app_log_emit(ESP_LOG_INFO, tag, #event, __VA_ARGS__)
#define APP_LOGD(tag, event, ...) app_log_emit(ESP_LOG_DEBUG, tag, #event, __VA_ARGS__)
#define APP_LOGV(tag, event, ...) app_log_emit(ESP_LOG_VERBOSE, tag, #event, __VA_ARGS__)
