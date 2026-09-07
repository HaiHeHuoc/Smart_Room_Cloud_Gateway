#pragma once

/** @file app_log.h
 * Task-context producer interface. No filesystem I/O or per-record allocation.
 * Not ISR/cache-disabled/panic safe. Console may block like normal ESP-IDF logs;
 * persistence never waits for the ring mutex. Never pass secrets as arguments.
 * Tags/events should be short static identifiers; details should use key=value.
 */
#include <stdbool.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif
bool app_log_should_emit(esp_log_level_t level);
void app_log_emit(esp_log_level_t level, const char *tag, const char *event,
                  const char *format, ...) __attribute__((format(printf, 4, 5)));
#ifdef __cplusplus
}
#endif

/* Event tokens are stringified; no enum/registration is needed. The frontend
 * guard preserves lazy argument evaluation when every configured sink rejects
 * the requested level. */
#define APP_LOG_IMPL(level, tag, event, ...)                         \
    do {                                                             \
        if (app_log_should_emit(level)) {                            \
            app_log_emit(level, tag, #event, __VA_ARGS__);           \
        }                                                            \
    } while (0)

#define APP_LOGE(tag, event, ...) APP_LOG_IMPL(ESP_LOG_ERROR, tag, event, __VA_ARGS__)
#define APP_LOGW(tag, event, ...) APP_LOG_IMPL(ESP_LOG_WARN, tag, event, __VA_ARGS__)
#define APP_LOGI(tag, event, ...) APP_LOG_IMPL(ESP_LOG_INFO, tag, event, __VA_ARGS__)
#define APP_LOGD(tag, event, ...) APP_LOG_IMPL(ESP_LOG_DEBUG, tag, event, __VA_ARGS__)
#define APP_LOGV(tag, event, ...) APP_LOG_IMPL(ESP_LOG_VERBOSE, tag, event, __VA_ARGS__)
