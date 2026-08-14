#ifndef TEST_HOST_ESP_LOG_H
#define TEST_HOST_ESP_LOG_H

#include "esp_err.h"

#define ESP_LOGI(tag, format, ...) \
    do                              \
    {                               \
        (void)(tag);                \
        (void)(format);             \
    } while (0)

#define ESP_LOGW(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)

static inline const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host-test-error";
}

#endif /* TEST_HOST_ESP_LOG_H */
