/* Includes ----------------------------------------------------------------- */
#include "app_reset_coordinator.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"

#include "app_gui.h"
#include "app_network_coordinator.h"
#include "config_manager.h"
#include "wifi_manager.h"

/* Macros ------------------------------------------------------------------- */
#define APP_RESET_COORDINATOR_QUEUE_LENGTH           4U
#define APP_RESET_COORDINATOR_TASK_STACK_SIZE_BYTES  3072U
#define APP_RESET_COORDINATOR_TASK_PRIORITY          4U
#define APP_RESET_COORDINATOR_TASK_NAME              "app_reset"

#define APP_RESET_COORDINATOR_UI_READY_TIMEOUT_MS      500U
#define APP_RESET_COORDINATOR_UI_POLL_PERIOD_MS         25U
#define APP_RESET_COORDINATOR_SUCCESS_DWELL_MS        1500U
#define APP_RESET_COORDINATOR_FALLBACK_DWELL_MS        500U
#define APP_RESET_COORDINATOR_NETWORK_QUIESCE_TIMEOUT_MS \
    10000U

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

static esp_err_t app_reset_coordinator_show_result(
    uint32_t transaction_id,
    ui_reset_state_t state,
    esp_err_t last_error);

static bool app_reset_coordinator_wait_for_reset_result(
    uint32_t transaction_id);

static void app_reset_coordinator_restart_after_success(
    uint32_t transaction_id,
    bool reset_result_queued);

/* Static Functions --------------------------------------------------------- */
static esp_err_t app_reset_coordinator_show_result(
    uint32_t transaction_id,
    ui_reset_state_t state,
    esp_err_t last_error)
{
    const ui_reset_status_t status =
    {
        .transaction_id =
            transaction_id,

        .state =
            state,

        .last_error =
            last_error,
    };

    return app_gui_show_reset_result(
        &status);
}

static bool app_reset_coordinator_wait_for_reset_result(
    uint32_t transaction_id)
{
    const TickType_t start_tick =
        xTaskGetTickCount();

    const TickType_t timeout_ticks =
        pdMS_TO_TICKS(
            APP_RESET_COORDINATOR_UI_READY_TIMEOUT_MS);

    const TickType_t poll_ticks =
        pdMS_TO_TICKS(
            APP_RESET_COORDINATOR_UI_POLL_PERIOD_MS);

    while ((TickType_t)(
               xTaskGetTickCount() -
               start_tick) < timeout_ticks)
    {
        bool presented = false;

        const esp_err_t error =
            app_gui_is_reset_result_presented(
                transaction_id,
                &presented);

        if (error != ESP_OK)
        {
            ESP_LOGW(
                TAG,
                "Failed to inspect reset-result presentation: %s",
                esp_err_to_name(error));

            return false;
        }

        if (presented)
        {
            return true;
        }

        vTaskDelay(
            poll_ticks);
    }

    return false;
}

static void app_reset_coordinator_restart_after_success(
    uint32_t transaction_id,
    bool reset_result_queued)
{
    bool presentation_confirmed = false;

    if (reset_result_queued)
    {
        presentation_confirmed =
            app_reset_coordinator_wait_for_reset_result(
                transaction_id);

        if (!presentation_confirmed)
        {
            ESP_LOGW(
                TAG,
                "Reset-result presentation was not confirmed: "
                "transaction=%lu",
                (unsigned long)transaction_id);
        }
    }

    const uint32_t dwell_ms =
        presentation_confirmed
            ? APP_RESET_COORDINATOR_SUCCESS_DWELL_MS
            : APP_RESET_COORDINATOR_FALLBACK_DWELL_MS;

    vTaskDelay(
        pdMS_TO_TICKS(
            dwell_ms));

    ESP_LOGW(
        TAG,
        "Restarting after verified Wi-Fi reset: transaction=%lu",
        (unsigned long)transaction_id);

    esp_restart();
}

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

    uint32_t transaction_counter = 0U;

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

                    if (transaction_counter == UINT32_MAX)
                    {
                        transaction_counter = 1U;
                    }
                    else
                    {
                        transaction_counter++;
                    }

                    const uint32_t transaction_id =
                        transaction_counter;

                    ESP_LOGI(
                        TAG,
                        "Factory-reset request accepted: transaction=%lu",
                        (unsigned long)transaction_id);

                    /*
                     * Storage work runs in the reset coordinator task.
                     *
                     * It does not execute in the button callback and therefore
                     * cannot block button_manager's polling task.
                     */
                    const esp_err_t preparation_error =
                        app_network_coordinator_prepare_for_factory_reset(
                            APP_RESET_COORDINATOR_NETWORK_QUIESCE_TIMEOUT_MS);

                    if (preparation_error != ESP_OK)
                    {
                        const esp_err_t ui_error =
                            app_reset_coordinator_show_result(
                                transaction_id,
                                UI_RESET_STATE_FAILED,
                                preparation_error);

                        if (ui_error != ESP_OK)
                        {
                            ESP_LOGW(
                                TAG,
                                "Failed to display reset preparation failure: "
                                "%s",
                                esp_err_to_name(ui_error));
                        }

                        ESP_LOGE(
                            TAG,
                            "Factory-reset network preparation failed: %s",
                            esp_err_to_name(preparation_error));

                        break;
                    }

                    /*
                     * The network coordinator now prevents any later
                     * provisioning persistence until reboot. Clear driver
                     * persistence before application configuration so a
                     * partial failure never destroys the recoverable NVS copy.
                     */
                    const esp_err_t driver_reset_error =
                        wifi_manager_clear_persistent_driver_settings();

                    if (driver_reset_error != ESP_OK)
                    {
                        const esp_err_t ui_error =
                            app_reset_coordinator_show_result(
                                transaction_id,
                                UI_RESET_STATE_FAILED,
                                driver_reset_error);

                        if (ui_error != ESP_OK)
                        {
                            ESP_LOGW(
                                TAG,
                                "Failed to display driver reset failure: %s",
                                esp_err_to_name(ui_error));
                        }

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
                        const esp_err_t ui_error =
                            app_reset_coordinator_show_result(
                                transaction_id,
                                UI_RESET_STATE_FAILED,
                                reset_error);

                        if (ui_error != ESP_OK)
                        {
                            ESP_LOGW(
                                TAG,
                                "Failed to display storage reset failure: %s",
                                esp_err_to_name(ui_error));
                        }

                        ESP_LOGE(
                            TAG,
                            "Factory-reset storage transaction failed: %s",
                            esp_err_to_name(reset_error));

                        break;
                    }

                    ESP_LOGI(
                        TAG,
                        "Factory reset verified; preparing controlled "
                        "restart: transaction=%lu",
                        (unsigned long)transaction_id);

                    const esp_err_t ui_error =
                        app_reset_coordinator_show_result(
                            transaction_id,
                            UI_RESET_STATE_SUCCESS,
                            ESP_OK);

                    const bool reset_result_queued =
                        ui_error == ESP_OK;

                    if (!reset_result_queued)
                    {
                        ESP_LOGW(
                            TAG,
                            "Failed to queue reset success UI: %s",
                            esp_err_to_name(ui_error));
                    }

                    app_reset_coordinator_restart_after_success(
                        transaction_id,
                        reset_result_queued);
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
        xQueueCreateWithCaps(
            APP_RESET_COORDINATOR_QUEUE_LENGTH,
            sizeof(app_reset_coordinator_input_event_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

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
