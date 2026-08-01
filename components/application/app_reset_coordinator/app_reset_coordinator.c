/* Includes ----------------------------------------------------------------- */
#include "app_reset_coordinator.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

/* Macros ------------------------------------------------------------------- */
#define APP_RESET_COORDINATOR_QUEUE_LENGTH          4U
#define APP_RESET_COORDINATOR_TASK_STACK_SIZE_BYTES 3072U
#define APP_RESET_COORDINATOR_TASK_PRIORITY         4U
#define APP_RESET_COORDINATOR_TASK_NAME             "app_reset"

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "APP_RESET_COORD";

/* Type Definitions --------------------------------------------------------- */
typedef enum
{
    APP_RESET_COORDINATOR_LIFECYCLE_UNINITIALIZED = 0,
    APP_RESET_COORDINATOR_LIFECYCLE_INITIALIZED,
    APP_RESET_COORDINATOR_LIFECYCLE_RUNNING
} app_reset_coordinator_lifecycle_t;

/**
 * @brief One-shot qualification state for one physical press cycle.
 */
typedef enum
{
    /**
     * No active press cycle.
     *
     * A PRESSED event begins a new cycle.
     */
    APP_RESET_COORDINATOR_STATE_ARMED = 0,

    /**
     * A stable press was received.
     *
     * One LONG_PRESS event may now be accepted.
     */
    APP_RESET_COORDINATOR_STATE_PRESS_ACTIVE,

    /**
     * The current press cycle already produced a reset request.
     *
     * Further LONG_PRESS events are ignored until RELEASED.
     */
    APP_RESET_COORDINATOR_STATE_REQUEST_ACCEPTED
} app_reset_coordinator_state_t;

typedef struct
{
    app_reset_coordinator_lifecycle_t lifecycle;

    QueueHandle_t input_queue;
    TaskHandle_t task_handle;
} app_reset_coordinator_context_t;

/* Static Variables --------------------------------------------------------- */
static app_reset_coordinator_context_t s_reset_coordinator =
    {
        .lifecycle =
            APP_RESET_COORDINATOR_LIFECYCLE_UNINITIALIZED,

        .input_queue =
            NULL,

        .task_handle =
            NULL,
};

/* Function Prototypes ------------------------------------------------------ */
static void app_reset_coordinator_task(
    void *argument);

static bool app_reset_coordinator_is_valid_input_event(
    app_reset_coordinator_input_event_t event);

/* Static Functions --------------------------------------------------------- */
static void app_reset_coordinator_task(
    void *argument)
{
    (void)argument;

    app_reset_coordinator_state_t state =
        APP_RESET_COORDINATOR_STATE_ARMED;

    while (true)
    {
        app_reset_coordinator_input_event_t event =
            APP_RESET_COORDINATOR_INPUT_RELEASED;

        const BaseType_t receive_result =
            xQueueReceive(
                s_reset_coordinator.input_queue,
                &event,
                portMAX_DELAY);

        if (receive_result != pdPASS)
        {
            continue;
        }

        switch (event)
        {
        case APP_RESET_COORDINATOR_INPUT_PRESSED:
        {
            if (state ==
                APP_RESET_COORDINATOR_STATE_ARMED)
            {
                state =
                    APP_RESET_COORDINATOR_STATE_PRESS_ACTIVE;

                ESP_LOGI(
                    TAG,
                    "Factory-reset press cycle started");
            }
            else
            {
                ESP_LOGD(
                    TAG,
                    "Duplicate pressed event ignored");
            }

            break;
        }

        case APP_RESET_COORDINATOR_INPUT_LONG_PRESS:
        {
            if (state ==
                APP_RESET_COORDINATOR_STATE_PRESS_ACTIVE)
            {
                state =
                    APP_RESET_COORDINATOR_STATE_REQUEST_ACCEPTED;

                /*
                 * Phase 7.2 ends here.
                 *
                 * Phase 7.3 will replace this diagnostic action with the
                 * verified Wi-Fi configuration reset transaction.
                 */
                ESP_LOGI(
                    TAG,
                    "Factory-reset request accepted "
                    "(Phase 7.2 diagnostic only)");
            }
            else if (state ==
                     APP_RESET_COORDINATOR_STATE_REQUEST_ACCEPTED)
            {
                ESP_LOGD(
                    TAG,
                    "Duplicate long-press request ignored");
            }
            else
            {
                ESP_LOGW(
                    TAG,
                    "Out-of-order long-press event ignored");
            }

            break;
        }

        case APP_RESET_COORDINATOR_INPUT_RELEASED:
        {
            if (state !=
                APP_RESET_COORDINATOR_STATE_ARMED)
            {
                ESP_LOGI(
                    TAG,
                    "Factory-reset input re-armed");
            }

            /*
             * Release always ends the current physical press cycle.
             */
            state =
                APP_RESET_COORDINATOR_STATE_ARMED;

            break;
        }

        default:
        {
            /*
             * Public validation should prevent this branch.
             */
            ESP_LOGW(
                TAG,
                "Unknown reset input event ignored");

            break;
        }
        }
    }
}

static bool app_reset_coordinator_is_valid_input_event(
    app_reset_coordinator_input_event_t event)
{
    switch (event)
    {
    case APP_RESET_COORDINATOR_INPUT_PRESSED:
    case APP_RESET_COORDINATOR_INPUT_LONG_PRESS:
    case APP_RESET_COORDINATOR_INPUT_RELEASED:
        return true;

    default:
        return false;
    }
}
/* Functions ---------------------------------------------------------------- */
esp_err_t app_reset_coordinator_init(void)
{
    if (s_reset_coordinator.lifecycle !=
        APP_RESET_COORDINATOR_LIFECYCLE_UNINITIALIZED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    QueueHandle_t input_queue =
        xQueueCreate(
            APP_RESET_COORDINATOR_QUEUE_LENGTH,
            sizeof(app_reset_coordinator_input_event_t));

    if (input_queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_reset_coordinator.input_queue =
        input_queue;

    s_reset_coordinator.task_handle =
        NULL;

    s_reset_coordinator.lifecycle =
        APP_RESET_COORDINATOR_LIFECYCLE_INITIALIZED;

    ESP_LOGI(
        TAG,
        "Reset coordinator initialized");

    return ESP_OK;
}

esp_err_t app_reset_coordinator_start(void)
{
    if ((s_reset_coordinator.lifecycle !=
         APP_RESET_COORDINATOR_LIFECYCLE_INITIALIZED) ||
        (s_reset_coordinator.input_queue == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Publish RUNNING before task creation because the newly created task may
     * be scheduled immediately after xTaskCreate() succeeds.
     */
    s_reset_coordinator.lifecycle =
        APP_RESET_COORDINATOR_LIFECYCLE_RUNNING;

    const BaseType_t task_result =
        xTaskCreate(
            app_reset_coordinator_task,
            APP_RESET_COORDINATOR_TASK_NAME,
            APP_RESET_COORDINATOR_TASK_STACK_SIZE_BYTES,
            NULL,
            APP_RESET_COORDINATOR_TASK_PRIORITY,
            &s_reset_coordinator.task_handle);

    if (task_result != pdPASS)
    {
        s_reset_coordinator.task_handle =
            NULL;

        s_reset_coordinator.lifecycle =
            APP_RESET_COORDINATOR_LIFECYCLE_INITIALIZED;

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Reset coordinator task started");

    return ESP_OK;
}

esp_err_t app_reset_coordinator_post_input_event(
    app_reset_coordinator_input_event_t event)
{
    if (!app_reset_coordinator_is_valid_input_event(event))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if ((s_reset_coordinator.lifecycle !=
         APP_RESET_COORDINATOR_LIFECYCLE_RUNNING) ||
        (s_reset_coordinator.input_queue == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Zero waiting time keeps the button callback non-blocking.
     */
    const BaseType_t queue_result =
        xQueueSend(
            s_reset_coordinator.input_queue,
            &event,
            0U);

    if (queue_result != pdPASS)
    {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
