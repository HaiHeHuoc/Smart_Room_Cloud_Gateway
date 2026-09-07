/* Includes ----------------------------------------------------------------- */
#include "app_reset_coordinator.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "app_log.h"
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
            APP_LOGW(
                TAG, FAILED_TO_INSPECT_RESET_RESU_D6953CED,
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
            APP_LOGW(
                TAG, RESET_RESULT_PRESENTATION_WA_6724DECD,
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

    APP_LOGW(
        TAG, RESTARTING_AFTER_VERIFIED_WI_91DFF49E,
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
        APP_LOGE(
            TAG, FAILED_TO_CLEAR_STORED_WI_6146ACE8,
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
        APP_LOGE(
            TAG, FAILED_TO_VERIFY_STORED_WI_1EBBF6A9,
            "Failed to verify stored Wi-Fi configuration: %s",
            esp_err_to_name(error));

        return error;
    }

    if (wifi_state !=
        CONFIG_MANAGER_WIFI_CONFIG_STATE_NOT_CONFIGURED)
    {
        APP_LOGE(
            TAG, WI_FI_CONFIGURATION_VERIFICA_E9B74989,
            "Wi-Fi configuration verification failed: state=%s",
            app_reset_coordinator_wifi_state_to_string(
                wifi_state));

        return ESP_ERR_INVALID_STATE;
    }

    APP_LOGI(
        TAG, WI_FI_CONFIGURATION_RESET_VE_E7F7B88D,
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

                    APP_LOGI(
                        TAG, FACTORY_RESET_PRESS_CYCLE_ST_BA23FAED,
                        "Factory-reset press cycle started");
                }
                else
                {
                    APP_LOGD(
                        TAG, DUPLICATE_PRESSED_EVENT_IGNO_3BE4DF2E,
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

                    APP_LOGI(
                        TAG, FACTORY_RESET_REQUEST_ACCEPT_589AB558,
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
                            APP_LOGW(
                                TAG, FAILED_TO_DISPLAY_RESET_PREP_4D3D1AAF,
                                "Failed to display reset preparation failure: "
                                "%s",
                                esp_err_to_name(ui_error));
                        }

                        APP_LOGE(
                            TAG, FACTORY_RESET_NETWORK_PREPAR_0F7D2F4E,
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
                            APP_LOGW(
                                TAG, FAILED_TO_DISPLAY_DRIVER_RES_A4C9F2E3,
                                "Failed to display driver reset failure: %s",
                                esp_err_to_name(ui_error));
                        }

                        APP_LOGE(
                            TAG, FACTORY_RESET_WI_FI_DRIVER_498B26D0,
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
                            APP_LOGW(
                                TAG, FAILED_TO_DISPLAY_STORAGE_RE_8666CA8C,
                                "Failed to display storage reset failure: %s",
                                esp_err_to_name(ui_error));
                        }

                        APP_LOGE(
                            TAG, FACTORY_RESET_STORAGE_TRANSA_A7122CAA,
                            "Factory-reset storage transaction failed: %s",
                            esp_err_to_name(reset_error));

                        break;
                    }

                    APP_LOGI(
                        TAG, FACTORY_RESET_VERIFIED_PREPA_615EE04D,
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
                        APP_LOGW(
                            TAG, FAILED_TO_QUEUE_RESET_SUCCES_298A3B0A,
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
                    APP_LOGD(
                        TAG, DUPLICATE_LONG_PRESS_REQUEST_83135323,
                        "Duplicate long-press request ignored");
                }
                else
                {
                    APP_LOGW(
                        TAG, OUT_OF_ORDER_LONG_PRESS_631CD313,
                        "Out-of-order long-press event ignored");
                }

                break;
            }

            case APP_RESET_COORDINATOR_INPUT_RELEASED:
            {
                if (state !=
                    APP_RESET_COORDINATOR_STATE_ARMED)
                {
                    APP_LOGI(
                        TAG, FACTORY_RESET_INPUT_RE_ARMED_015447FE,
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
                APP_LOGW(
                    TAG, UNKNOWN_RESET_INPUT_EVENT_IG_9BE04D48,
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

    APP_LOGI(
        TAG, RESET_COORDINATOR_INITIALIZE_8E34EBDA,
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

    APP_LOGI(
        TAG, RESET_COORDINATOR_TASK_START_F15094F6,
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
