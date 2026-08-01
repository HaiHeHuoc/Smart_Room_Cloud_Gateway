/* Includes ----------------------------------------------------------------- */
#include "button_manager.h"

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

/* Macros ------------------------------------------------------------------- */
#define BUTTON_MANAGER_TASK_STACK_SIZE_BYTES  3072U
#define BUTTON_MANAGER_TASK_PRIORITY          4U
#define BUTTON_MANAGER_TASK_NAME              "button_manager"

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "BUTTON_MANAGER";

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    BUTTON_MANAGER_LIFECYCLE_UNINITIALIZED = 0,
    BUTTON_MANAGER_LIFECYCLE_INITIALIZED,
    BUTTON_MANAGER_LIFECYCLE_RUNNING
} button_manager_lifecycle_t;

typedef struct
{
    button_manager_lifecycle_t lifecycle;

    button_manager_config_t config;

    button_manager_event_callback_t callback;
    void *user_context;

    TaskHandle_t task_handle;
} button_manager_context_t;
/* Static Variables --------------------------------------------------------- */
static button_manager_context_t s_button_manager = {
    .lifecycle = BUTTON_MANAGER_LIFECYCLE_UNINITIALIZED,
    .config = {0},
    .callback = NULL,
    .user_context = NULL,
    .task_handle = NULL
};

/* Function Prototypes ------------------------------------------------------ */
static esp_err_t button_manager_validate_config(
    const button_manager_config_t *config);

static void button_manager_task(
    void *argument);

static bool button_manager_read_pressed(void);

static void button_manager_publish_event(
    button_manager_event_t event,
    TickType_t held_ticks);

static uint32_t button_manager_ticks_to_ms(
    TickType_t ticks);

/* Static Functions --------------------------------------------------------- */
static bool button_manager_read_pressed(void)
{
    return gpio_get_level(
               s_button_manager.config.gpio_num) ==
           (int)s_button_manager.config.active_level;
}

static void button_manager_publish_event(
    button_manager_event_t event,
    TickType_t held_ticks)
{
    button_manager_event_data_t event_data =
    {
        .event =
            event,

        .held_ms =
            button_manager_ticks_to_ms(
                held_ticks),
    };

    /*
     * No manager mutex is held. The callback runs directly in the button task
     * and must return quickly.
     */
    s_button_manager.callback(
        &event_data,
        s_button_manager.user_context);
}

static uint32_t button_manager_ticks_to_ms(
    TickType_t ticks)
{
    const uint64_t milliseconds =
        ((uint64_t)ticks * 1000ULL) /
        (uint64_t)configTICK_RATE_HZ;

    if (milliseconds > UINT32_MAX)
    {
        return UINT32_MAX;
    }

    return (uint32_t)milliseconds;
}

static void button_manager_task(
    void *argument)
{
    (void)argument;

    const TickType_t poll_ticks =
        pdMS_TO_TICKS(
            s_button_manager.config.poll_period_ms);

    const TickType_t debounce_ticks =
        pdMS_TO_TICKS(
            s_button_manager.config.debounce_ms);

    const TickType_t long_press_ticks =
        pdMS_TO_TICKS(
            s_button_manager.config.long_press_ms);

    TickType_t last_wake_tick =
        xTaskGetTickCount();

    TickType_t candidate_since_tick =
        last_wake_tick;

    TickType_t press_started_tick =
        last_wake_tick;

    bool stable_pressed =
        false;

    bool candidate_pressed =
        false;

    bool long_press_reported =
        false;

    while(true)
    {
        const TickType_t now_tick =
            xTaskGetTickCount();

        const bool raw_pressed =
            button_manager_read_pressed();

        if (raw_pressed != candidate_pressed)
        {
            candidate_pressed =
                raw_pressed;

            candidate_since_tick =
                now_tick;
        }

        if ((candidate_pressed != stable_pressed) &&
            ((TickType_t)(now_tick - candidate_since_tick) >=
             debounce_ticks))
        {
            stable_pressed =
                candidate_pressed;

            if (stable_pressed)
            {
                press_started_tick =
                    now_tick;

                long_press_reported =
                    false;

                button_manager_publish_event(
                    BUTTON_MANAGER_EVENT_PRESSED,
                    0U);
            }
            else
            {
                const TickType_t held_ticks =
                    (TickType_t)(
                        candidate_since_tick -
                        press_started_tick);

                button_manager_publish_event(
                    BUTTON_MANAGER_EVENT_RELEASED,
                    held_ticks);

                long_press_reported =
                    false;
            }
        }

        if (stable_pressed &&
            candidate_pressed &&
            !long_press_reported)
        {
            const TickType_t held_ticks =
                (TickType_t)(
                    now_tick -
                    press_started_tick);

            if (held_ticks >= long_press_ticks)
            {
                long_press_reported =
                    true;
    
                button_manager_publish_event(
                    BUTTON_MANAGER_EVENT_LONG_PRESS,
                    held_ticks);
            }
        }

        vTaskDelayUntil(
            &last_wake_tick,
            poll_ticks);
    }
}

static esp_err_t button_manager_validate_config(
    const button_manager_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL,
        ESP_ERR_INVALID_ARG,
        TAG, 
        "Config is NULL");

    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(config->gpio_num),
        ESP_ERR_INVALID_ARG,
        TAG, 
        "Invalid GPIO number");

    ESP_RETURN_ON_FALSE(config->active_level <= 1U,
        ESP_ERR_INVALID_ARG,
        TAG, 
        "Invalid active level");

    ESP_RETURN_ON_FALSE(config->poll_period_ms > 0U,
        ESP_ERR_INVALID_ARG,
        TAG, 
        "Poll period must be greater than zero");

    ESP_RETURN_ON_FALSE(config->debounce_ms > config->poll_period_ms,
        ESP_ERR_INVALID_ARG,
        TAG, 
        "Debounce time must be greater than poll period");

    ESP_RETURN_ON_FALSE(config->poll_period_ms < config->long_press_ms,
        ESP_ERR_INVALID_ARG,
        TAG, 
        "Long press time must be greater than poll period");

    if ((pdMS_TO_TICKS(config->poll_period_ms) == 0U) ||
        (pdMS_TO_TICKS(config->debounce_ms) == 0U) ||
        (pdMS_TO_TICKS(config->long_press_ms) == 0U))
    {
        ESP_LOGE(
            TAG,
            "Poll, debounce, and long press times must be greater than 0"
        );
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}
/* Functions ---------------------------------------------------------------- */
esp_err_t button_manager_init(
    const button_manager_config_t *config)
{
    if (s_button_manager.lifecycle !=
        BUTTON_MANAGER_LIFECYCLE_UNINITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret =
        button_manager_validate_config(
            config);

    if (ret != ESP_OK)
    {
        return ret;
    }

    gpio_config_t gpio_configuration =
    {
        .pin_bit_mask =
            1ULL << (uint32_t)config->gpio_num,

        .mode =
            GPIO_MODE_INPUT,

        .pull_up_en =
            config->active_level == 0U
                ? GPIO_PULLUP_ENABLE
                : GPIO_PULLUP_DISABLE,

        .pull_down_en =
            config->active_level == 0U
                ? GPIO_PULLDOWN_DISABLE
                : GPIO_PULLDOWN_ENABLE,

        .intr_type =
            GPIO_INTR_DISABLE,
    };

    ret =
        gpio_config(
            &gpio_configuration);

    if (ret != ESP_OK)
    {
        return ret;
    }

    s_button_manager.config =
        *config;

    s_button_manager.callback =
        NULL;

    s_button_manager.user_context =
        NULL;

    s_button_manager.task_handle =
        NULL;

    s_button_manager.lifecycle =
        BUTTON_MANAGER_LIFECYCLE_INITIALIZED;

    ESP_LOGI(
        TAG,
        "Initialized: gpio=%d, active_level=%u, poll=%u ms, "
        "debounce=%u ms, long_press=%u ms",
        (int)config->gpio_num,
        (unsigned int)config->active_level,
        (unsigned int)config->poll_period_ms,
        (unsigned int)config->debounce_ms,
        (unsigned int)config->long_press_ms);

    return ESP_OK;
}

esp_err_t button_manager_register_callback(
    button_manager_event_callback_t callback,
    void *user_context)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_button_manager.lifecycle !=
        BUTTON_MANAGER_LIFECYCLE_INITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_button_manager.callback =
        callback;

    s_button_manager.user_context =
        user_context;

    return ESP_OK;
}

esp_err_t button_manager_start(void)
{
    if ((s_button_manager.lifecycle !=
         BUTTON_MANAGER_LIFECYCLE_INITIALIZED) ||
        (s_button_manager.callback == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_button_manager.lifecycle =
        BUTTON_MANAGER_LIFECYCLE_RUNNING;

    BaseType_t task_result =
        xTaskCreate(
            button_manager_task,
            BUTTON_MANAGER_TASK_NAME,
            BUTTON_MANAGER_TASK_STACK_SIZE_BYTES,
            NULL,
            BUTTON_MANAGER_TASK_PRIORITY,
            &s_button_manager.task_handle);

    if (task_result != pdPASS)
    {
        s_button_manager.task_handle =
            NULL;

        s_button_manager.lifecycle =
            BUTTON_MANAGER_LIFECYCLE_INITIALIZED;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Polling task started");

    return ESP_OK;
}
