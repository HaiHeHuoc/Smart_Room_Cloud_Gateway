#include "light_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "neopixel.h"

#define LIGHT_MANAGER_LOCK_TIMEOUT_MS 1000U

static const char *const TAG = "LIGHT_MANAGER";

typedef struct
{
    bool initialized;
    light_manager_state_t state;
} light_manager_context_t;

static light_manager_context_t s_context;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_lock_bootstrap_guard = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t light_manager_get_lock(void)
{
    taskENTER_CRITICAL(&s_lock_bootstrap_guard);

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }

    const SemaphoreHandle_t lock = s_lock;

    taskEXIT_CRITICAL(&s_lock_bootstrap_guard);

    return lock;
}

static esp_err_t light_manager_take_lock(void)
{
    const SemaphoreHandle_t lock = light_manager_get_lock();

    if (lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    return xSemaphoreTake(lock, pdMS_TO_TICKS(LIGHT_MANAGER_LOCK_TIMEOUT_MS))
                   == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void light_manager_give_lock(void)
{
    xSemaphoreGive(s_lock);
}

static bool light_manager_config_is_valid(const light_manager_config_t *config)
{
    return (config != NULL) &&
           (config->led_count > 0U) &&
           (config->default_brightness_percent <= 100U) &&
           (config->pixel_format <= LIGHT_MANAGER_PIXEL_FORMAT_GRB) &&
           GPIO_IS_VALID_OUTPUT_GPIO(config->gpio_num);
}

static uint32_t light_manager_state_to_rgb(const light_manager_state_t *state)
{
    return ((uint32_t)state->red << 16U) |
           ((uint32_t)state->green << 8U) |
           (uint32_t)state->blue;
}

static esp_err_t light_manager_apply_state_locked(
    const light_manager_state_t *state)
{
    const esp_err_t ret = neopixel_set_static_state(
        light_manager_state_to_rgb(state),
        state->brightness_percent,
        state->power_on);

    if (ret == ESP_OK)
    {
        s_context.state = *state;
    }

    return ret;
}

esp_err_t light_manager_init(const light_manager_config_t *config)
{
    if (!light_manager_config_is_valid(config))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    const neopixel_config_t neopixel_config =
    {
        .gpio_num = config->gpio_num,
        .led_count = config->led_count,
        .format = config->pixel_format == LIGHT_MANAGER_PIXEL_FORMAT_RGB
                      ? NEOPIXEL_FORMAT_RGB
                      : NEOPIXEL_FORMAT_GRB,
        .default_brightness = config->default_brightness_percent,
    };

    esp_err_t ret = neopixel_init(&neopixel_config);
    if (ret == ESP_OK)
    {
        const light_manager_state_t initial_state =
        {
            .power_on = false,
            .red = 0U,
            .green = 0U,
            .blue = 0U,
            .brightness_percent = config->default_brightness_percent,
        };

        ret = light_manager_apply_state_locked(&initial_state);
        if (ret != ESP_OK)
        {
            const esp_err_t deinit_ret = neopixel_deinit();
            if (deinit_ret != ESP_OK)
            {
                ESP_LOGE(
                    TAG,
                    "NeoPixel cleanup after initialization failure failed: %s",
                    esp_err_to_name(deinit_ret));
            }
        }
        else
        {
            s_context.initialized = true;
            ESP_LOGI(
                TAG,
                "Initialized: gpio=%d leds=%u format=%s boot=OFF",
                (int)config->gpio_num,
                (unsigned)config->led_count,
                config->pixel_format == LIGHT_MANAGER_PIXEL_FORMAT_RGB
                    ? "RGB"
                    : "GRB");
        }
    }

    light_manager_give_lock();
    return ret;
}

esp_err_t light_manager_deinit(void)
{
    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = neopixel_deinit();
    if (ret == ESP_OK)
    {
        memset(&s_context, 0, sizeof(s_context));
        ESP_LOGI(TAG, "Deinitialized");
    }

    light_manager_give_lock();
    return ret;
}

esp_err_t light_manager_set_state(const light_manager_state_t *state)
{
    if ((state == NULL) || (state->brightness_percent > 100U))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = light_manager_apply_state_locked(state);

    light_manager_give_lock();
    return ret;
}

esp_err_t light_manager_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    light_manager_state_t state = s_context.state;
    state.red = red;
    state.green = green;
    state.blue = blue;

    const esp_err_t ret = light_manager_apply_state_locked(&state);

    light_manager_give_lock();
    return ret;
}

esp_err_t light_manager_set_brightness(uint8_t brightness_percent)
{
    if (brightness_percent > 100U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    light_manager_state_t state = s_context.state;
    state.brightness_percent = brightness_percent;

    const esp_err_t ret = light_manager_apply_state_locked(&state);

    light_manager_give_lock();
    return ret;
}

esp_err_t light_manager_on(void)
{
    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    light_manager_state_t state = s_context.state;
    state.power_on = true;

    const esp_err_t ret = light_manager_apply_state_locked(&state);

    light_manager_give_lock();
    return ret;
}

esp_err_t light_manager_off(void)
{
    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    light_manager_state_t state = s_context.state;
    state.power_on = false;

    const esp_err_t ret = light_manager_apply_state_locked(&state);

    light_manager_give_lock();
    return ret;
}

esp_err_t light_manager_get_state(light_manager_state_t *state)
{
    if (state == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    *state = s_context.state;

    light_manager_give_lock();
    return ESP_OK;
}
