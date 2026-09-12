#include "light_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "neopixel.h"
#include "sdkconfig.h"

#define LIGHT_MANAGER_LOCK_TIMEOUT_MS 1000U
#define LIGHT_MANAGER_BLINK_ON_TIME_MS 500U
#define LIGHT_MANAGER_BLINK_OFF_TIME_MS 500U
#define LIGHT_MANAGER_BREATH_PERIOD_MS 2000U
#define LIGHT_MANAGER_PULSE_DURATION_MS 300U
#define LIGHT_MANAGER_PULSE_REPEAT_COUNT UINT32_MAX
#define LIGHT_MANAGER_RAINBOW_STEP_MS 10U

#if CONFIG_LIGHT_MANAGER_TEST_LOOP
#define LIGHT_MANAGER_TEST_TASK_STACK_SIZE 4096U
#define LIGHT_MANAGER_TEST_TASK_PRIORITY   4U
#endif

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

#if CONFIG_LIGHT_MANAGER_TEST_LOOP
static TaskHandle_t s_test_task;
#endif

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

static bool light_manager_effect_is_valid(light_manager_effect_t effect)
{
    return effect <= LIGHT_MANAGER_EFFECT_RAINBOW;
}

static esp_err_t light_manager_start_effect_locked(
    const light_manager_state_t *state)
{
    const uint32_t color = light_manager_state_to_rgb(state);
    esp_err_t ret = neopixel_set_brightness(state->brightness_percent);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (state->effect) {
    case LIGHT_MANAGER_EFFECT_BLINK:
        ret = neopixel_blink(color, state->brightness_percent,
                             LIGHT_MANAGER_BLINK_ON_TIME_MS,
                             LIGHT_MANAGER_BLINK_OFF_TIME_MS);
        break;
    case LIGHT_MANAGER_EFFECT_BREATH:
        ret = neopixel_breath(color, LIGHT_MANAGER_BREATH_PERIOD_MS);
        break;
    case LIGHT_MANAGER_EFFECT_PULSE:
        ret = neopixel_pulse(color, state->brightness_percent,
                             LIGHT_MANAGER_PULSE_DURATION_MS,
                             LIGHT_MANAGER_PULSE_REPEAT_COUNT);
        break;
    case LIGHT_MANAGER_EFFECT_RAINBOW:
        ret = neopixel_rainbow_cycle(LIGHT_MANAGER_RAINBOW_STEP_MS);
        break;
    case LIGHT_MANAGER_EFFECT_SOLID:
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if ((ret == ESP_OK) && !state->power_on) {
        ret = neopixel_off();
    }
    return ret;
}

static esp_err_t light_manager_apply_state_locked(
    const light_manager_state_t *state)
{
    if (!light_manager_effect_is_valid(state->effect)) {
        return ESP_ERR_INVALID_ARG;
    }

    const light_manager_state_t previous = s_context.state;
    const bool state_unchanged =
        (state->power_on == previous.power_on) &&
        (state->red == previous.red) &&
        (state->green == previous.green) &&
        (state->blue == previous.blue) &&
        (state->brightness_percent == previous.brightness_percent) &&
        (state->effect == previous.effect);
    const bool only_power_changed =
        (state->effect != LIGHT_MANAGER_EFFECT_SOLID) &&
        (state->effect == previous.effect) &&
        (state->red == previous.red) &&
        (state->green == previous.green) &&
        (state->blue == previous.blue) &&
        (state->brightness_percent == previous.brightness_percent);
    esp_err_t ret = ESP_OK;

    if (state_unchanged) {
        ret = ESP_OK;
    } else if (state->effect == LIGHT_MANAGER_EFFECT_SOLID) {
        ret = neopixel_set_static_state(light_manager_state_to_rgb(state),
                                        state->brightness_percent,
                                        state->power_on);
    } else if (only_power_changed && (state->power_on != previous.power_on)) {
        ret = state->power_on ? neopixel_on() : neopixel_off();
    } else {
        ret = light_manager_start_effect_locked(state);
    }

    if (ret == ESP_OK)
    {
        s_context.state = *state;
    }

    return ret;
}

#if CONFIG_LIGHT_MANAGER_TEST_LOOP
static const char *const s_test_effect_names[] =
{
    "solid",
    "blink",
    "breath",
    "pulse",
    "rainbow",
};

static void light_manager_test_wait(void)
{
    vTaskDelay(pdMS_TO_TICKS(CONFIG_LIGHT_MANAGER_TEST_STEP_DELAY_MS));
}

static void light_manager_test_log_result(
    const char *step,
    esp_err_t result)
{
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "LIGHT_TEST: %s: PASS", step);
    }
    else
    {
        ESP_LOGE(
            TAG,
            "LIGHT_TEST: %s: FAIL: %s",
            step,
            esp_err_to_name(result));
    }
}

static void light_manager_test_run_action(
    const char *step,
    esp_err_t result)
{
    ESP_LOGI(TAG, "LIGHT_TEST: testing %s", step);
    light_manager_test_log_result(step, result);
    light_manager_test_wait();
}

static void light_manager_test_log_state(void)
{
    light_manager_state_t state = {0};
    const esp_err_t ret = light_manager_get_state(&state);

    if (ret == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "LIGHT_TEST: get_state: PASS power=%d rgb=(%u,%u,%u) brightness=%u effect=%s",
            state.power_on,
            (unsigned)state.red,
            (unsigned)state.green,
            (unsigned)state.blue,
            (unsigned)state.brightness_percent,
            state.effect <= LIGHT_MANAGER_EFFECT_RAINBOW
                ? s_test_effect_names[state.effect]
                : "invalid");
    }
    else
    {
        light_manager_test_log_result("get_state", ret);
    }

    light_manager_test_wait();
}

static void light_manager_test_task(void *context)
{
    (void)context;

    const light_manager_state_t magenta =
    {
        .power_on = true,
        .red = 255U,
        .green = 0U,
        .blue = 255U,
        .brightness_percent = 100U,
        .effect = LIGHT_MANAGER_EFFECT_SOLID,
    };

    for (;;)
    {
        ESP_LOGI(TAG, "LIGHT_TEST: starting a new complete test cycle");

        for (size_t index = 0U; index < LIGHT_MANAGER_EFFECT_RAINBOW + 1U; ++index) {
            light_manager_state_t state = magenta;
            state.effect = (light_manager_effect_t)index;
            light_manager_test_run_action(s_test_effect_names[index],
                                          light_manager_set_state(&state));
            light_manager_test_log_state();
        }

        light_manager_test_run_action("solid restore", light_manager_set_state(&magenta));
        light_manager_test_log_state();

        light_manager_state_t blink = magenta;
        blink.effect = LIGHT_MANAGER_EFFECT_BLINK;
        light_manager_test_run_action("blink before off/on", light_manager_set_state(&blink));
        light_manager_test_run_action("off (preserve blink)", light_manager_off());
        light_manager_test_log_state();
        light_manager_test_run_action("on (resume blink)", light_manager_on());
        light_manager_test_log_state();
    }
}
#endif

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
            .effect = LIGHT_MANAGER_EFFECT_SOLID,
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

#if CONFIG_LIGHT_MANAGER_TEST_LOOP
    if (s_test_task != NULL)
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }
#endif

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
    if ((state == NULL) || (state->brightness_percent > 100U) ||
        !light_manager_effect_is_valid(state->effect))
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

esp_err_t light_manager_start_test_loop(void)
{
#if !CONFIG_LIGHT_MANAGER_TEST_LOOP
    return ESP_ERR_NOT_SUPPORTED;
#else
    const esp_err_t lock_ret = light_manager_take_lock();
    if (lock_ret != ESP_OK)
    {
        return lock_ret;
    }

    if (!s_context.initialized || (s_test_task != NULL))
    {
        light_manager_give_lock();
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(
            light_manager_test_task,
            "light_test",
            LIGHT_MANAGER_TEST_TASK_STACK_SIZE,
            NULL,
            LIGHT_MANAGER_TEST_TASK_PRIORITY,
            &s_test_task) != pdPASS)
    {
        s_test_task = NULL;
        light_manager_give_lock();
        return ESP_ERR_NO_MEM;
    }

    light_manager_give_lock();

    ESP_LOGW(
        TAG,
        "Infinite target test loop started: step_delay_ms=%u",
        (unsigned)CONFIG_LIGHT_MANAGER_TEST_STEP_DELAY_MS);
    return ESP_OK;
#endif
}
