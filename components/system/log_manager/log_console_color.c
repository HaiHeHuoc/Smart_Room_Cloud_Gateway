/* Includes ----------------------------------------------------------------- */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_log_color.h"
#include "esp_log_write.h"

/* Macros ------------------------------------------------------------------- */
#define APP_LOG_CONSOLE_SINK_TAG "APP_LOG_SINK"
#define APP_LOG_COLOR_FORMAT_BYTES 64U

/* Function Prototypes ------------------------------------------------------ */
void log_manager_console_write(
    esp_log_level_t level,
    const char *tag,
    const char *format,
    ...);

/* Static Functions --------------------------------------------------------- */
static const char *console_color_for_level(esp_log_level_t level)
{
    switch (level)
    {
        case ESP_LOG_ERROR:
            return LOG_COLOR_E;

        case ESP_LOG_WARN:
            return LOG_COLOR_W;

        case ESP_LOG_INFO:
            return LOG_COLOR_I;

        case ESP_LOG_DEBUG:
            return LOG_COLOR_D;

        case ESP_LOG_VERBOSE:
            return LOG_COLOR_V;

        case ESP_LOG_NONE:
        default:
            return "";
    }
}

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Console-only wrapper for log_manager's direct esp_log_write() call.
 *
 * log_manager.c is compiled with esp_log_write renamed to this symbol. Only the
 * reserved APP_LOG console sink receives color framing; any other direct call
 * is forwarded unchanged. The persistent record itself never contains ANSI
 * escape sequences because coloring happens after the structured line has
 * already been formatted and queued for storage.
 */
void log_manager_console_write(
    esp_log_level_t level,
    const char *tag,
    const char *format,
    ...)
{
    va_list args;
    va_start(args, format);

    if ((tag != NULL) &&
        (format != NULL) &&
        (strcmp(tag, APP_LOG_CONSOLE_SINK_TAG) == 0))
    {
        const char *const color =
            console_color_for_level(level);

        if ((color != NULL) && (color[0] != '\0'))
        {
            char colored_format[APP_LOG_COLOR_FORMAT_BYTES];
            const int written =
                snprintf(
                    colored_format,
                    sizeof(colored_format),
                    "%s%s%s",
                    color,
                    format,
                    LOG_RESET_COLOR);

            if ((written > 0) &&
                ((size_t)written < sizeof(colored_format)))
            {
                esp_log_writev(
                    level,
                    tag,
                    colored_format,
                    args);
                va_end(args);
                return;
            }
        }
    }

    esp_log_writev(
        level,
        tag,
        format,
        args);

    va_end(args);
}
