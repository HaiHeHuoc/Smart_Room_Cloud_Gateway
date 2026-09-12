#include "light_manager.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "neopixel.h"
#include "sdkconfig.h"

#define LIGHT_MANAGER_LOCK_TIMEOUT_MS 1000U

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

#if CONFIG_LIGHT_MANAGER_TEST_LOOP
typedef enum
{
    LIGHT_MANAGER_TEST_EFFECT_BLINK = 0,
    LIGHT_MANAGER_TEST_EFFECT_FADE_IN,
    LIGHT_MANAGER_TEST_EFFECT_FADE_OUT,
    LIGHT_MANAGER_TEST_EFFECT_FADE,
    LIGHT_MANAGER_TEST_EFFECT_TRANSITION,
    LIGHT_MANAGER_TEST_EFFECT_RAINBOW,
    LIGHT_MANAGER_TEST_EFFECT_RAINBOW_CYCLE,
    LIGHT_MANAGER_TEST_EFFECT_BREATH,
    LIGHT_MANAGER_TEST_EFFECT_PULSE,
    LIGHT_MANAGER_TEST_EFFECT_CHASE,
    LIGHT_MANAGER_TEST_EFFECT_COLOR_WIPE,
    LIGHT_MANAGER_TEST_EFFECT_THEATER_CHASE,
    LIGHT_MANAGER_TEST_EFFECT_GRADIENT,
    LIGHT_MANAGER_TEST_EFFECT_RAINBOW_GRADIENT,
    LIGHT_MANAGER_TEST_EFFECT_COUNT,
} light_manager_test_effect_t;

static const char *const s_test_effect_names[] =
{
    "effect blink",
    "effect fade_in",
    "effect fade_out",
    "effect fade",
    "effect transition",
    "effect rainbow",
    "effect rainbow_cycle",
    "effect breath",
    "effect pulse",
    "effect chase",
    "effect color_wipe",
    "effect theater_chase",
    "effect gradient",
    "effect rainbow_gradient",
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

static esp_err_t light_manager_test_start_effect(
    light_manager_test_effect_t effect)
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

    esp_err_t ret = ESP_ERR_INVALID_ARG;

    switch (effect)
    {
        case LIGHT_MANAGER_TEST_EFFECT_BLINK:
            ret = neopixel_blink(NEOPIXEL_COLOR_RED, 100U, 1000U, 1000U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_FADE_IN:
            ret = neopixel_fade_in(NEOPIXEL_COLOR_MAGENTA, 4000U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_FADE_OUT:
            ret = neopixel_fade_out(4000U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_FADE:
            ret = neopixel_fade(NEOPIXEL_COLOR_GREEN, 0U, 100U, 4000U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_TRANSITION:
            ret = neopixel_transition(
                NEOPIXEL_COLOR_RED,
                NEOPIXEL_COLOR_BLUE,
                4000U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_RAINBOW:
            ret = neopixel_rainbow(6000U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_RAINBOW_CYCLE:
            ret = neopixel_rainbow_cycle(100U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_BREATH:
            ret = neopixel_breath(NEOPIXEL_COLOR_CYAN, 4000U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_PULSE:
            ret = neopixel_pulse(NEOPIXEL_COLOR_YELLOW, 100U, 1000U, 3U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_CHASE:
            ret = neopixel_chase(NEOPIXEL_COLOR_ORANGE, 200U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_COLOR_WIPE:
            ret = neopixel_color_wipe(NEOPIXEL_COLOR_PURPLE, 300U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_THEATER_CHASE:
            ret = neopixel_theater_chase(NEOPIXEL_COLOR_WHITE, 200U);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_GRADIENT:
            ret = neopixel_gradient(NEOPIXEL_COLOR_RED, NEOPIXEL_COLOR_BLUE);
            break;

        case LIGHT_MANAGER_TEST_EFFECT_RAINBOW_GRADIENT:
            ret = neopixel_rainbow_gradient();
            break;

        case LIGHT_MANAGER_TEST_EFFECT_COUNT:
        default:
            break;
    }

    light_manager_give_lock();
    return ret;
}

static esp_err_t light_manager_test_effect_control(
    const char *operation)
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

    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (strcmp(operation, "pause") == 0)
    {
        ret = neopixel_pause_effect();
    }
    else if (strcmp(operation, "resume") == 0)
    {
        ret = neopixel_resume_effect();
    }
    else if (strcmp(operation, "stop") == 0)
    {
        ret = neopixel_stop_effect();
    }

    light_manager_give_lock();
    return ret;
}

static void light_manager_test_log_state(void)
{
    light_manager_state_t state = {0};
    const esp_err_t ret = light_manager_get_state(&state);

    if (ret == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "LIGHT_TEST: get_state: PASS power=%d rgb=(%u,%u,%u) brightness=%u",
            state.power_on,
            (unsigned)state.red,
            (unsigned)state.green,
            (unsigned)state.blue,
            (unsigned)state.brightness_percent);
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
    };

    for (;;)
    {
        ESP_LOGI(TAG, "LIGHT_TEST: starting a new complete test cycle");

        light_manager_test_run_action(
            "set_state magenta at 100 percent",
            light_manager_set_state(&magenta));
        light_manager_test_log_state();
        light_manager_test_run_action(
            "set_color green",
            light_manager_set_color(0U, 255U, 0U));
        light_manager_test_run_action(
            "set_brightness 20 percent",
            light_manager_set_brightness(20U));
        light_manager_test_run_action("off", light_manager_off());
        light_manager_test_run_action("on (restore green at 20 percent)", light_manager_on());
        light_manager_test_run_action(
            "set_brightness 0 percent",
            light_manager_set_brightness(0U));
        light_manager_test_run_action(
            "set_brightness 100 percent",
            light_manager_set_brightness(100U));

        for (size_t index = 0U;
             index < LIGHT_MANAGER_TEST_EFFECT_COUNT;
             ++index)
        {
            const light_manager_test_effect_t effect =
                (light_manager_test_effect_t)index;

            light_manager_test_run_action(
                s_test_effect_names[index],
                light_manager_test_start_effect(effect));

            if (effect == LIGHT_MANAGER_TEST_EFFECT_BLINK)
            {
                light_manager_test_run_action(
                    "effect pause",
                    light_manager_test_effect_control("pause"));
                light_manager_test_run_action(
                    "effect resume",
                    light_manager_test_effect_control("resume"));
            }

            light_manager_test_run_action(
                "effect stop",
                light_manager_test_effect_control("stop"));
        }
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
