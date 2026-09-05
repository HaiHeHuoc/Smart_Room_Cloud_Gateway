#include "voice_assistant_ptt_gpio.h"

#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "voice_assistant_ptt.h"

#define PTT_GPIO_TASK_NAME        "voice_ptt_gpio"
#define PTT_GPIO_TASK_STACK_BYTES 3072U
#define PTT_GPIO_TASK_PRIORITY    4U

static const char *const TAG = "VOICE_PTT_GPIO";

static voice_assistant_ptt_gpio_config_t s_config = {0};
static TaskHandle_t s_task = NULL;
static bool s_initialized = false;

static bool ptt_gpio_is_pressed(void)
{
    const int level = gpio_get_level(s_config.gpio_num);
    return (level == (int)s_config.active_level);
}

static void ptt_gpio_task(void *argument)
{
    (void)argument;

    bool stable = ptt_gpio_is_pressed();
    bool candidate = stable;
    TickType_t candidate_since = xTaskGetTickCount();

    ESP_LOGI(TAG,
             "started gpio=%d active_level=%u pull=down initial=%s",
             (int)s_config.gpio_num,
             (unsigned)s_config.active_level,
             stable ? "pressed" : "released");

    for (;;) {
        const bool sampled = ptt_gpio_is_pressed();
        const TickType_t now = xTaskGetTickCount();

        if (sampled != candidate) {
            candidate = sampled;
            candidate_since = now;
        } else if ((candidate != stable) &&
                   ((now - candidate_since) >=
                    pdMS_TO_TICKS(s_config.debounce_ms))) {
            stable = candidate;
            const esp_err_t ret = stable
                                      ? voice_assistant_ptt_press()
                                      : voice_assistant_ptt_release();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "edge=%s", stable ? "PRESS" : "RELEASE");
            } else {
                ESP_LOGW(TAG,
                         "edge=%s rejected: %s",
                         stable ? "PRESS" : "RELEASE",
                         esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_config.poll_period_ms));
    }
}

esp_err_t voice_assistant_ptt_gpio_init(
    const voice_assistant_ptt_gpio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_OK;
    }
    if (!GPIO_IS_VALID_GPIO(config->gpio_num) ||
        (config->active_level > 1U) ||
        (config->poll_period_ms == 0U) ||
        (config->debounce_ms < config->poll_period_ms)) {
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << (uint32_t)config->gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t ret = gpio_config(&io_config);
    if (ret != ESP_OK) {
        return ret;
    }

    s_config = *config;
    s_initialized = true;
    ESP_LOGI(TAG,
             "initialized gpio=%d active_level=%u poll=%ums debounce=%ums",
             (int)s_config.gpio_num,
             (unsigned)s_config.active_level,
             (unsigned)s_config.poll_period_ms,
             (unsigned)s_config.debounce_ms);
    return ESP_OK;
}

esp_err_t voice_assistant_ptt_gpio_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }

    if (xTaskCreate(
            ptt_gpio_task,
            PTT_GPIO_TASK_NAME,
            PTT_GPIO_TASK_STACK_BYTES,
            NULL,
            PTT_GPIO_TASK_PRIORITY,
            &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
