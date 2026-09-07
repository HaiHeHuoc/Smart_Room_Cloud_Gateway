/* Includes ----------------------------------------------------------------- */
#include "app_log.h"
#include "app_log_backend.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

/* Static Variables --------------------------------------------------------- */
static app_log_sink_t s_sink;
static app_log_filter_t s_filter;

/* Functions ---------------------------------------------------------------- */
void app_log_register_backend(app_log_sink_t sink, app_log_filter_t filter)
{
    __atomic_store_n(&s_filter, filter, __ATOMIC_RELEASE);
    __atomic_store_n(&s_sink, sink, __ATOMIC_RELEASE);
}

bool app_log_should_emit(esp_log_level_t level)
{
    if (level <= ESP_LOG_NONE || level > ESP_LOG_VERBOSE) {
        return false;
    }

    app_log_filter_t filter =
        __atomic_load_n(&s_filter, __ATOMIC_ACQUIRE);

    /* Before composition installs a backend, preserve the early console
     * fallback rather than dropping startup diagnostics. */
    return (filter == NULL) || filter(level);
}

void app_log_emit(esp_log_level_t level, const char *tag, const char *event,
                  const char *format, ...)
{
    if (xPortInIsrContext() || !tag || !event || !format ||
        level <= ESP_LOG_NONE || level > ESP_LOG_VERBOSE) return;
    va_list args;
    va_start(args, format);
    app_log_sink_t sink = __atomic_load_n(&s_sink, __ATOMIC_ACQUIRE);
    if (sink) {
        sink(level, tag, event, format, args);
    } else {
        /* Very early fallback is console-only, bounded and one physical line. */
        char text[256] = {0};
        vsnprintf(text, sizeof(text) - 1, format, args);
        size_t n = strlen(text);
        for (size_t i = 0; i < n; ++i) if ((unsigned char)text[i] < 32) text[i] = ' ';
        text[n++] = '\n'; text[n] = 0;
        esp_log_write(level, tag, "[%.24s][%.40s] %s", tag, event, text);
    }
    va_end(args);
}
