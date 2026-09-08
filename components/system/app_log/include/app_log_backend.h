#pragma once
/* Backend registration only; ordinary producers include app_log.h instead.
 * Handler code must remain valid for application lifetime. Registration is
 * composition-owned, task-context only; no SD/time dependency lives here. */
#include <stdbool.h>
#include <stdarg.h>
#include "esp_log_level.h"

typedef void (*app_log_sink_t)(esp_log_level_t level, const char *tag,
                               const char *event, const char *format, va_list args);
typedef bool (*app_log_filter_t)(esp_log_level_t level);

void app_log_register_backend(app_log_sink_t sink, app_log_filter_t filter);
