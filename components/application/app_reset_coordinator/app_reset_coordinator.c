/* Includes ----------------------------------------------------------------- */
#include "app_reset_coordinator.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "config_manager.h"
#include "wifi_manager.h"

/* Macros ------------------------------------------------------------------- */
#define APP_RESET_COORDINATOR_QUEUE_LENGTH           4U
#define APP_RESET_COORDINATOR_TASK_STACK_SIZE_BYTES  3072U
#define APP_RESET_COORDINATOR_TASK_PRIORITY          4U
#define APP_RESET_COORDINATOR_TASK_NAME              "app_reset"
#define APP_RESET_COORDINATOR_RESTART_DELAY_MS       500U

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

/**
 * @brief Erase only stored Wi-Fi credentials and verify persistent state.
 *
 * @return ESP_OK only when the resulting persistent state is
 *         CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED.
 */
static esp_err_t app_reset_coordinator_clear_and_verify_wifi(void);

/**
 * @brief Return a diagnostic string for a Wi-Fi configuration state.
 */
static const char *app_reset_coordinator_wifi_state_to_string(
    config_manager_wifi_config_state_t state);

/* Static Functions --------------------------------------------------------- */
static const char *app_reset_coordinator_wifi_state_to_string(
    config_manager_wifi_config_state_t state)
{
    switch (state)
    {
        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN:
            return "UNKNOWN";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED:
            return "NOT_CONFIGURED";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_VALID:
            return "VALID";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INCOMPLETE:
            return "INCOMPLETE";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_UNSUPPORTED_VERSION:
            return "UNSUPPORTED_VERSION";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_INVALID_DATA:
            return "INVALID_DATA";

        case CONFIG_MANAGER_WIFI_CONFIG_STATE_MIGRATION_REQUIRED:
            return "MIGRATION_REQUIRED";

        default:
            return "INVALID_ENUM";
    }
}

static esp_err_t app_reset_coordinator_clear_and_verify_wifi(void)
{
    /*
     * config_manager_clear_wifi() owns NVS locking, handle lifecycle,
     * credential-key erasure, and commit.
     *
     * It preserves configuration version, custom data, and device identity.
     */
    esp_err_t error =
        config_manager_clear_wifi();

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to clear stored Wi-Fi configuration: %s",
            esp_err_to_name(error));

        return error;
    }

    config_manager_wifi_config_state_t wifi_state =
        CONFIG_MANAGER_WIFI_CONFIG_STATE_UNKNOWN;

    /*
     * Do not trust only the erase return value. Reopen and classify the
     * persistent configuration after the committed operation.
     */
    error =
        config_manager_get_wifi_config_state(
            &wifi_state);

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to verify stored Wi-Fi configuration: %s",
            esp_err_to_name(error));

        return error;
    }

    if (wifi_state !=
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi configuration verification failed: state=%s",
            app_reset_coordinator_wifi_state_to_string(
                wifi_state));

        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Wi-Fi configuration reset verified: state=%s",
        app_reset_coordinator_wifi_state_to_string(
            wifi_state));

    return ESP_OK;
}

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
                    /*
                     * Lock this physical press cycle before starting storage
                     * work. Any duplicate queued LONG_PRESS event is ignored.
                     */
                    state =
                        APP_RESET_COORDINATOR_STATE_REQUEST_ACCEPTED;

                    ESP_LOGI(
                        TAG,
                        "Factory-reset request accepted");

                    /*
                     * Storage work runs in the reset coordinator task.
                     *
                     * It does not execute in the button callback and therefore
                     * cannot block button_manager's polling task.
                     */
                    /*
                     * Clear driver-owned persistence first. If Wi-Fi is not
                     * initialized yet, application configuration remains
                     * valid and the reset can be retried safely.
                     */
                    const esp_err_t driver_reset_error =
                        wifi_manager_clear_persistent_driver_settings();

                    if (driver_reset_error != ESP_OK)
                    {
                        /*
                         * Application configuration is still intact, so an
                         * early request or driver failure cannot leave the
                         * authoritative credential store erased.
                         */
                        ESP_LOGE(
                            TAG,
                            "Factory-reset Wi-Fi driver cleanup failed: %s",
                            esp_err_to_name(driver_reset_error));

                        break;
                    }

                    const esp_err_t reset_error =
                        app_reset_coordinator_clear_and_verify_wifi();

                    if (reset_error != ESP_OK)
                    {
                        /*
                         * Driver cleanup is idempotent. Preserve runtime and
                         * suppress reboot so the application operation can be
                         * retried after release.
                         */
                        ESP_LOGE(
                            TAG,
                            "Factory-reset storage transaction failed: %s",
                            esp_err_to_name(reset_error));

                        break;
                    }

                    ESP_LOGW(
                        TAG,
                        "Factory reset verified; restarting into provisioning");

                    /*
                     * Give the serial transport a finite opportunity to emit
                     * the terminal diagnostic before rebooting. Runtime Wi-Fi
                     * and DHCP state are discarded by esp_restart().
                     */
                    vTaskDelay(
                        pdMS_TO_TICKS(
                            APP_RESET_COORDINATOR_RESTART_DELAY_MS));

                    esp_restart();
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
                 * A failed transaction does not reboot. Release re-arms the
                 * coordinator so the idempotent cleanup can be retried.
                 * Successful transactions restart before this event is read.
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
