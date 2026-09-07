/* Includes ----------------------------------------------------------------- */
#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#include "esp_log.h"
#include "app_log.h"
#include "esp_check.h"
/* Macros ------------------------------------------------------------------- */

#define WIFI_MANAGER_RECONNECT_TASK_NAME           "wifi_reconnect"

#define WIFI_MANAGER_RECONNECT_TASK_STACK_SIZE     4096U
#define WIFI_MANAGER_RECONNECT_TASK_PRIORITY       4U

#define WIFI_MANAGER_RECONNECT_INITIAL_DELAY_MS    1000U
#define WIFI_MANAGER_RECONNECT_MAX_DELAY_MS        60000U

#define WIFI_MANAGER_CONNECTION_TIMEOUT_MS         30000U
#define WIFI_MANAGER_DRIVER_OPERATION_TIMEOUT_MS   5000U
#define WIFI_MANAGER_MS_TO_US                      1000ULL

/* Constants ---------------------------------------------------------------- */

static const char *const TAG = "WIFI_MANAGER";

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief Internal state owned by wifi_manager.
 *
 * This structure is private. Other components can only obtain a copy through
 * wifi_manager_get_status().
 */
typedef struct
{
    bool initialized;

    /*
     * True after wifi_manager_connect() has successfully applied
     * a Station configuration to the ESP-IDF Wi-Fi driver.
     */
    bool credentials_configured;

    /*
     * Controls whether an unexpected disconnection may start
     * the automatic reconnect flow.
     */
    bool auto_reconnect_enabled;

    /*
     * Distinguishes application-requested disconnect from
     * router/AP/network failures.
     */
    bool manual_disconnect_requested;

    /*
     * Current exponential-backoff delay.
     */
    uint32_t reconnect_delay_ms;

    /*
     * Number of reconnect attempts since the last successful GOT_IP.
     */
    uint32_t reconnect_attempt_count;

    esp_netif_t *station_netif;

    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;

    /*
     * Created during wifi_manager_init().
     */
    TaskHandle_t reconnect_task_handle;

    /*
     * One-shot watchdog for one complete Station connection attempt:
     * esp_wifi_connect() -> association -> DHCP -> GOT_IP.
     */
    esp_timer_handle_t connection_timeout_timer;

    /*
     * Attempt identity and deadline prevent a delayed timer callback from an
     * older attempt from aborting a newer connection attempt.
     */
    uint32_t connection_attempt_generation;
    uint32_t connection_timeout_generation;
    int64_t connection_attempt_deadline_us;

    bool connection_attempt_active;
    bool connection_timeout_pending;
    bool connection_timeout_abort_in_progress;

    /*
     * True only while provisioning cleanup is detaching a Station attempt
     * that has not been adopted by wifi_manager. This transient guard rejects
     * a late GOT_IP event without enabling persistent manual-disconnect
     * policy or automatic reconnect.
     */
    bool unmanaged_disconnect_in_progress;

    wifi_manager_status_t status;

    wifi_manager_status_callback_t status_callback;

    void *status_callback_user_data;
} wifi_manager_context_t;

/* Static Variables --------------------------------------------------------- */
static wifi_manager_context_t s_wifi_manager = {
    .initialized = false,

    .credentials_configured = false,
    .auto_reconnect_enabled = false,
    .manual_disconnect_requested = false,

    .reconnect_delay_ms =
        WIFI_MANAGER_RECONNECT_INITIAL_DELAY_MS,

    .reconnect_attempt_count = 0U,

    .station_netif = NULL,

    .wifi_event_instance = NULL,
    .ip_event_instance = NULL,

    .reconnect_task_handle = NULL,

    .connection_timeout_timer = NULL,
    .connection_attempt_generation = 0U,
    .connection_timeout_generation = 0U,
    .connection_attempt_deadline_us = 0,
    .connection_attempt_active = false,
    .connection_timeout_pending = false,
    .connection_timeout_abort_in_progress = false,
    .unmanaged_disconnect_in_progress = false,

    .status = {
        .state = WIFI_MANAGER_STATE_UNINITIALIZED,
        .ssid = {0},
        .ipv4_address = {0},
        .rssi_dbm = 0,
        .disconnect_reason = 0U,
        .has_ipv4_address = false,
        .rssi_valid = false,
    },

    .status_callback = NULL,
    .status_callback_user_data = NULL,
};

/*
 * Protect shared Wi-Fi manager state accessed by:
 *
 * - ESP event-loop task;
 * - reconnect task;
 * - application/UI tasks.
 *
 * Protected fields include:
 *
 * - status;
 * - callback pointers;
 * - reconnect flags and counters;
 * - reconnect task and connection-attempt timeout state.
 */
static portMUX_TYPE s_status_lock =
    portMUX_INITIALIZER_UNLOCKED;

static StaticSemaphore_t s_driver_operation_mutex_storage;
static SemaphoreHandle_t s_driver_operation_mutex = NULL;


/* Function Prototypes ------------------------------------------------------ */
static void wifi_manager_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
);

static void wifi_manager_notify_status_changed(void);

static void wifi_manager_reconnect_task(
    void *argument);

static bool wifi_manager_schedule_reconnect(void);

static void wifi_manager_connection_timeout_callback(
    void *argument);

static esp_err_t wifi_manager_start_connection_attempt(void);

static esp_err_t wifi_manager_lock_driver_operation(void);

static void wifi_manager_unlock_driver_operation(void);

static void wifi_manager_cancel_connection_attempt_locked(void);

static esp_err_t wifi_manager_stop_connection_timeout_timer(void);

static bool wifi_manager_process_connection_timeout(void);

/* Static Functions --------------------------------------------------------- */
static void wifi_manager_cancel_connection_attempt_locked(void)
{
    s_wifi_manager.connection_attempt_active = false;
    s_wifi_manager.connection_timeout_pending = false;
    s_wifi_manager.connection_timeout_generation = 0U;
    s_wifi_manager.connection_attempt_deadline_us = 0;
}

static void wifi_manager_zeroize(
    void *buffer,
    size_t size)
{
    volatile uint8_t *cursor =
        (volatile uint8_t *)buffer;

    while ((cursor != NULL) &&
           (size > 0U))
    {
        *cursor = 0U;
        cursor++;
        size--;
    }
}

static esp_err_t wifi_manager_lock_driver_operation(void)
{
    if (s_driver_operation_mutex == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t timeout_ticks =
        pdMS_TO_TICKS(
            WIFI_MANAGER_DRIVER_OPERATION_TIMEOUT_MS);

    if (xSemaphoreTake(
            s_driver_operation_mutex,
            timeout_ticks) != pdTRUE)
    {
        APP_LOGE(
            TAG, TIMED_OUT_WAITING_FOR_WI_A35B6DA1,
            "Timed out waiting for Wi-Fi driver operation lock");

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void wifi_manager_unlock_driver_operation(void)
{
    if (s_driver_operation_mutex != NULL)
    {
        (void)xSemaphoreGive(s_driver_operation_mutex);
    }
}

static esp_err_t wifi_manager_stop_connection_timeout_timer(void)
{
    esp_timer_handle_t timer = NULL;

    taskENTER_CRITICAL(&s_status_lock);
    timer = s_wifi_manager.connection_timeout_timer;
    taskEXIT_CRITICAL(&s_status_lock);

    if (timer == NULL)
    {
        return ESP_OK;
    }

    const esp_err_t error = esp_timer_stop(timer);

    /* Stopping an already stopped one-shot timer is an expected race. */
    if (error == ESP_ERR_INVALID_STATE)
    {
        return ESP_OK;
    }

    return error;
}

static void wifi_manager_connection_timeout_callback(
    void *argument)
{
    (void)argument;

    TaskHandle_t reconnect_task_handle = NULL;
    const int64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_status_lock);

    const bool connection_state_active =
        s_wifi_manager.status.state == WIFI_MANAGER_STATE_CONNECTING ||
        s_wifi_manager.status.state == WIFI_MANAGER_STATE_WAITING_FOR_IP;

    if (s_wifi_manager.initialized &&
        s_wifi_manager.connection_attempt_active &&
        !s_wifi_manager.connection_timeout_pending &&
        !s_wifi_manager.connection_timeout_abort_in_progress &&
        !s_wifi_manager.status.has_ipv4_address &&
        connection_state_active &&
        now_us >= s_wifi_manager.connection_attempt_deadline_us)
    {
        s_wifi_manager.connection_timeout_pending = true;
        s_wifi_manager.connection_timeout_generation =
            s_wifi_manager.connection_attempt_generation;

        reconnect_task_handle =
            s_wifi_manager.reconnect_task_handle;
    }

    taskEXIT_CRITICAL(&s_status_lock);

    /* The timer runs in ESP_TIMER_TASK context, not in an ISR. */
    if (reconnect_task_handle != NULL)
    {
        xTaskNotifyGive(reconnect_task_handle);
    }
}

static esp_err_t wifi_manager_start_connection_attempt(void)
{
    esp_timer_handle_t timer = NULL;
    uint32_t attempt_generation = 0U;

    taskENTER_CRITICAL(&s_status_lock);

    const bool can_start =
        s_wifi_manager.initialized &&
        s_wifi_manager.credentials_configured &&
        s_wifi_manager.auto_reconnect_enabled &&
        !s_wifi_manager.manual_disconnect_requested &&
        !s_wifi_manager.status.has_ipv4_address &&
        !s_wifi_manager.connection_attempt_active &&
        !s_wifi_manager.connection_timeout_abort_in_progress &&
        s_wifi_manager.status.state == WIFI_MANAGER_STATE_CONNECTING &&
        s_wifi_manager.connection_timeout_timer != NULL;

    timer = s_wifi_manager.connection_timeout_timer;

    taskEXIT_CRITICAL(&s_status_lock);

    if (!can_start)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = wifi_manager_stop_connection_timeout_timer();

    if (error != ESP_OK)
    {
        return error;
    }

    const int64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_status_lock);

    const bool still_can_start =
        s_wifi_manager.initialized &&
        s_wifi_manager.credentials_configured &&
        s_wifi_manager.auto_reconnect_enabled &&
        !s_wifi_manager.manual_disconnect_requested &&
        !s_wifi_manager.status.has_ipv4_address &&
        !s_wifi_manager.connection_attempt_active &&
        !s_wifi_manager.connection_timeout_abort_in_progress &&
        s_wifi_manager.status.state == WIFI_MANAGER_STATE_CONNECTING;

    if (still_can_start)
    {
        s_wifi_manager.connection_attempt_generation++;

        /* Keep zero reserved as the "no timeout pending" value. */
        if (s_wifi_manager.connection_attempt_generation == 0U)
        {
            s_wifi_manager.connection_attempt_generation = 1U;
        }

        attempt_generation =
            s_wifi_manager.connection_attempt_generation;

        s_wifi_manager.connection_attempt_active = true;
        s_wifi_manager.connection_timeout_pending = false;
        s_wifi_manager.connection_timeout_generation = 0U;
        s_wifi_manager.connection_attempt_deadline_us =
            now_us +
            ((int64_t)WIFI_MANAGER_CONNECTION_TIMEOUT_MS *
             (int64_t)WIFI_MANAGER_MS_TO_US);
    }

    taskEXIT_CRITICAL(&s_status_lock);

    if (!still_can_start)
    {
        return ESP_ERR_INVALID_STATE;
    }

    error = esp_timer_start_once(
        timer,
        (uint64_t)WIFI_MANAGER_CONNECTION_TIMEOUT_MS *
            WIFI_MANAGER_MS_TO_US);

    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_status_lock);

        if (s_wifi_manager.connection_attempt_generation ==
            attempt_generation)
        {
            wifi_manager_cancel_connection_attempt_locked();
        }

        taskEXIT_CRITICAL(&s_status_lock);

        return error;
    }

    error = wifi_manager_lock_driver_operation();

    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_status_lock);

        if (s_wifi_manager.connection_attempt_generation ==
            attempt_generation)
        {
            wifi_manager_cancel_connection_attempt_locked();
        }

        taskEXIT_CRITICAL(&s_status_lock);

        (void)wifi_manager_stop_connection_timeout_timer();
        return error;
    }

    /*
     * Manual disconnect or another terminal event may have won while the timer
     * or driver-operation lock was being acquired. Recheck while holding the
     * driver lock so a reset disconnect cannot return before a late connect
     * command is issued.
     */
    taskENTER_CRITICAL(&s_status_lock);

    const bool attempt_still_active =
        s_wifi_manager.connection_attempt_active &&
        s_wifi_manager.connection_attempt_generation ==
            attempt_generation &&
        !s_wifi_manager.manual_disconnect_requested &&
        !s_wifi_manager.status.has_ipv4_address;

    taskEXIT_CRITICAL(&s_status_lock);

    if (!attempt_still_active)
    {
        wifi_manager_unlock_driver_operation();
        (void)wifi_manager_stop_connection_timeout_timer();
        return ESP_ERR_INVALID_STATE;
    }

    error = esp_wifi_connect();
    wifi_manager_unlock_driver_operation();

    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_status_lock);

        if (s_wifi_manager.connection_attempt_generation ==
            attempt_generation)
        {
            wifi_manager_cancel_connection_attempt_locked();
        }

        taskEXIT_CRITICAL(&s_status_lock);

        (void)wifi_manager_stop_connection_timeout_timer();
    }

    return error;
}

static bool wifi_manager_process_connection_timeout(void)
{
    bool timeout_was_pending = false;
    bool timeout_is_current = false;
    uint32_t timeout_generation = 0U;
    const int64_t now_us = esp_timer_get_time();

    taskENTER_CRITICAL(&s_status_lock);

    timeout_was_pending =
        s_wifi_manager.connection_timeout_pending;

    if (timeout_was_pending)
    {
        timeout_generation =
            s_wifi_manager.connection_timeout_generation;

        const bool connection_state_active =
            s_wifi_manager.status.state == WIFI_MANAGER_STATE_CONNECTING ||
            s_wifi_manager.status.state == WIFI_MANAGER_STATE_WAITING_FOR_IP;

        timeout_is_current =
            s_wifi_manager.initialized &&
            s_wifi_manager.credentials_configured &&
            s_wifi_manager.auto_reconnect_enabled &&
            !s_wifi_manager.manual_disconnect_requested &&
            !s_wifi_manager.status.has_ipv4_address &&
            s_wifi_manager.connection_attempt_active &&
            !s_wifi_manager.connection_timeout_abort_in_progress &&
            connection_state_active &&
            timeout_generation ==
                s_wifi_manager.connection_attempt_generation &&
            now_us >= s_wifi_manager.connection_attempt_deadline_us;

        s_wifi_manager.connection_timeout_pending = false;
        s_wifi_manager.connection_timeout_generation = 0U;

        if (timeout_is_current)
        {
            s_wifi_manager.connection_attempt_active = false;
            s_wifi_manager.connection_attempt_deadline_us = 0;
            s_wifi_manager.connection_timeout_abort_in_progress = true;
        }
    }

    taskEXIT_CRITICAL(&s_status_lock);

    if (!timeout_was_pending)
    {
        return false;
    }

    if (!timeout_is_current)
    {
        APP_LOGD(
            TAG, IGNORED_STALE_WI_FI_CONNECTI_D37DCBA0,
            "Ignored stale Wi-Fi connection timeout, generation=%lu",
            (unsigned long)timeout_generation);

        return false;
    }

    (void)wifi_manager_stop_connection_timeout_timer();

    APP_LOGW(
        TAG, WI_FI_CONNECTION_DHCP_TIMED_A463877E,
        "Wi-Fi connection/DHCP timed out after %lu ms, generation=%lu",
        (unsigned long)WIFI_MANAGER_CONNECTION_TIMEOUT_MS,
        (unsigned long)timeout_generation);

    /*
     * A normal DISCONNECTED event may have completed between claiming the
     * timeout and reaching this task context. Avoid an unnecessary second
     * driver disconnect in that case.
     */
    taskENTER_CRITICAL(&s_status_lock);

    const bool abort_still_required =
        s_wifi_manager.connection_timeout_abort_in_progress;

    taskEXIT_CRITICAL(&s_status_lock);

    if (!abort_still_required)
    {
        return true;
    }

    esp_err_t error =
        wifi_manager_lock_driver_operation();

    if (error == ESP_OK)
    {
        error = esp_wifi_disconnect();
        wifi_manager_unlock_driver_operation();
    }

    if (error == ESP_OK)
    {
        /* WIFI_EVENT_STA_DISCONNECTED owns the normal retry scheduling. */
        return true;
    }

    bool manual_disconnect = false;

    taskENTER_CRITICAL(&s_status_lock);

    s_wifi_manager.connection_timeout_abort_in_progress = false;

    manual_disconnect =
        s_wifi_manager.manual_disconnect_requested;

    if (manual_disconnect)
    {
        s_wifi_manager.status.state =
            (error == ESP_ERR_WIFI_NOT_CONNECT)
                ? WIFI_MANAGER_STATE_DISCONNECTED
                : WIFI_MANAGER_STATE_FAILED;
        s_wifi_manager.status.has_ipv4_address = false;
        s_wifi_manager.status.ipv4_address[0] = '\0';
        s_wifi_manager.status.rssi_valid = false;
        s_wifi_manager.status.rssi_dbm = 0;
    }
    taskEXIT_CRITICAL(&s_status_lock);

    if (error == ESP_ERR_WIFI_NOT_CONNECT)
    {
        APP_LOGD(
            TAG, TIMED_OUT_WI_FI_ATTEMPT_EE4A0D47,
            "Timed-out Wi-Fi attempt was already detached");
    }
    else
    {
        APP_LOGW(
            TAG, COULD_NOT_ABORT_TIMED_OUT_2BCCE37C,
            "Could not abort timed-out Wi-Fi attempt: %s",
            esp_err_to_name(error));
    }

    if (manual_disconnect)
    {
        wifi_manager_notify_status_changed();
        return true;
    }

    if (!wifi_manager_schedule_reconnect())
    {
        APP_LOGW(
            TAG, TIMED_OUT_WI_FI_ATTEMPT_CA73C547,
            "Timed-out Wi-Fi attempt could not schedule reconnect");
    }

    return true;
}

static bool wifi_manager_schedule_reconnect(void)
{
    TaskHandle_t reconnect_task_handle = NULL;
    bool should_schedule = false;


    /*
     * Read reconnect policy and update state atomically.
     */
    taskENTER_CRITICAL(
        &s_status_lock);

    if (s_wifi_manager.initialized &&
        s_wifi_manager.credentials_configured &&
        s_wifi_manager.auto_reconnect_enabled &&
        !s_wifi_manager.manual_disconnect_requested &&
        !s_wifi_manager.status.has_ipv4_address &&
        !s_wifi_manager.connection_attempt_active &&
        !s_wifi_manager.connection_timeout_abort_in_progress &&
        s_wifi_manager.reconnect_task_handle != NULL)
    {
        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_RETRY_WAIT;

        reconnect_task_handle =
            s_wifi_manager.reconnect_task_handle;

        should_schedule = true;
    }

    taskEXIT_CRITICAL(
        &s_status_lock);

    if (!should_schedule)
    {
        return false;
    }

    /*
     * Notify application/GUI that Wi-Fi is waiting to retry.
     */
    wifi_manager_notify_status_changed();

    /*
     * ESP event handlers execute from a task context, not from an ISR,
     * so use the normal task-notification API.
     */
    xTaskNotifyGive(
        reconnect_task_handle);

    APP_LOGI(
        TAG, AUTOMATIC_WI_FI_RECONNECT_SC_EBC50B1E,
        "Automatic Wi-Fi reconnect scheduled");

    return true;
}

static void wifi_manager_reconnect_task(
    void *argument)
{
    (void)argument;

    APP_LOGI(
        TAG, WI_FI_RECONNECT_TASK_STARTED_6FE381E5,
        "Wi-Fi reconnect task started");

    while(1)
    {
        uint32_t notification_count =
            ulTaskNotifyTake(
                pdTRUE,
                portMAX_DELAY);

        /*
         * Timeout notifications share this task with regular reconnect
         * notifications, but timeout recovery must first abort the currently
         * active driver attempt.
         */
        if (wifi_manager_process_connection_timeout())
        {
            continue;
        }

        uint32_t reconnect_delay_ms = 0U;
        bool should_reconnect = false;

        /*
         * Copy reconnect policy under the lock.
         *
         * Do not hold the critical section while delaying,
         * logging or calling ESP-IDF Wi-Fi APIs.
         */
        taskENTER_CRITICAL(
            &s_status_lock);

        should_reconnect =
            s_wifi_manager.initialized &&
            s_wifi_manager.credentials_configured &&
            s_wifi_manager.auto_reconnect_enabled &&
            !s_wifi_manager.manual_disconnect_requested &&
            !s_wifi_manager.status.has_ipv4_address &&
            !s_wifi_manager.connection_attempt_active &&
            !s_wifi_manager.connection_timeout_abort_in_progress &&
            s_wifi_manager.status.state ==
                WIFI_MANAGER_STATE_RETRY_WAIT;

        reconnect_delay_ms =
            s_wifi_manager.reconnect_delay_ms;

        taskEXIT_CRITICAL(
            &s_status_lock);


        if (!should_reconnect)
        {
            APP_LOGD(
                TAG, RECONNECT_ATTEMPT_SKIPPED_465C07EF,
                "Reconnect attempt skipped");

            continue;
        }


        APP_LOGI(
            TAG, RETRYING_WI_FI_CONNECTION_IN_B554882E,
            "Retrying Wi-Fi connection in %lu ms",
            (unsigned long)reconnect_delay_ms);

        vTaskDelay(
            pdMS_TO_TICKS(
                reconnect_delay_ms));

        uint32_t reconnect_attempt = 0U;
        uint32_t next_reconnect_delay_ms = 0U;

        taskENTER_CRITICAL(
            &s_status_lock);

        should_reconnect =
            s_wifi_manager.initialized &&
            s_wifi_manager.credentials_configured &&
            s_wifi_manager.auto_reconnect_enabled &&
            !s_wifi_manager.manual_disconnect_requested &&
            !s_wifi_manager.status.has_ipv4_address &&
            !s_wifi_manager.connection_attempt_active &&
            !s_wifi_manager.connection_timeout_abort_in_progress &&
            s_wifi_manager.status.state ==
                WIFI_MANAGER_STATE_RETRY_WAIT;


        if (should_reconnect)
        {
            s_wifi_manager.reconnect_attempt_count++;

            reconnect_attempt =
                s_wifi_manager.reconnect_attempt_count;

            s_wifi_manager.status.state =
                WIFI_MANAGER_STATE_CONNECTING;

            
            /*
            * Prepare the delay for the next attempt.
            *
            * The current attempt has already waited using the old value.
            */
            if (s_wifi_manager.reconnect_delay_ms >=
                (WIFI_MANAGER_RECONNECT_MAX_DELAY_MS / 2U))
            {
                s_wifi_manager.reconnect_delay_ms =
                    WIFI_MANAGER_RECONNECT_MAX_DELAY_MS;
            }
            else
            {
                s_wifi_manager.reconnect_delay_ms *= 2U;
            }

            next_reconnect_delay_ms =
                s_wifi_manager.reconnect_delay_ms;
        }


        taskEXIT_CRITICAL(
            &s_status_lock);

        if (!should_reconnect)
        {
            APP_LOGD(
                TAG, RECONNECT_ATTEMPT_CANCELLED_D2B307AB,
                "Reconnect attempt cancelled");

            continue;
        }

        /*
         * Report CONNECTING to the GUI/application before invoking
         * the asynchronous Wi-Fi connection API.
         */
        wifi_manager_notify_status_changed();


        APP_LOGI(
            TAG, STARTING_WI_FI_RECONNECT_ATT_B7E2E1C0,
            "Starting Wi-Fi reconnect attempt %lu, "
            "next retry delay=%lu ms",
            (unsigned long)reconnect_attempt,
            (unsigned long)next_reconnect_delay_ms);

        esp_err_t error =
            wifi_manager_start_connection_attempt();

        if (error != ESP_OK)
        {
            bool retry_allowed = false;

            taskENTER_CRITICAL(
                &s_status_lock);

            retry_allowed =
                s_wifi_manager.initialized &&
                s_wifi_manager.credentials_configured &&
                s_wifi_manager.auto_reconnect_enabled &&
                !s_wifi_manager.manual_disconnect_requested &&
                !s_wifi_manager.status.has_ipv4_address;

            if (retry_allowed)
            {
                s_wifi_manager.status.state =
                    WIFI_MANAGER_STATE_FAILED;
            }

            taskEXIT_CRITICAL(
                &s_status_lock);

            if (!retry_allowed)
            {
                APP_LOGD(
                    TAG, WI_FI_RECONNECT_ATTEMPT_WAS_BBC5B547,
                    "Wi-Fi reconnect attempt was canceled");

                continue;
            }

            APP_LOGW(
                TAG, FAILED_TO_START_WI_FI_533CE756,
                "Failed to start Wi-Fi reconnect attempt: %s",
                esp_err_to_name(error));

            wifi_manager_notify_status_changed();

            /*
             * Schedule another attempt using the existing backoff delay.
             *
             * This is safe from the reconnect task itself:
             * the notification is consumed during the next loop.
             */
            if (!wifi_manager_schedule_reconnect())
            {
                APP_LOGW(
                    TAG, FAILED_TO_RESCHEDULE_WI_FI_786B80A5,
                    "Failed to reschedule Wi-Fi reconnect");
            }
        }


        APP_LOGD(
            TAG, RECONNECT_TASK_AWAKENED_NOTI_2D29872A,
            "Reconnect task awakened, notifications=%lu",
            (unsigned long)notification_count);
    }
}
static void wifi_manager_notify_status_changed(void)
{
    wifi_manager_status_t status_snapshot = {0};

    wifi_manager_status_callback_t callback = NULL;
    void *callback_user_data = NULL;

    /*
     * Copy all shared information while holding the lock.
     */
    taskENTER_CRITICAL(&s_status_lock);

    memcpy(
        &status_snapshot,
        &s_wifi_manager.status,
        sizeof(status_snapshot)
    );

    callback =
        s_wifi_manager.status_callback;

    callback_user_data =
        s_wifi_manager.status_callback_user_data;

    taskEXIT_CRITICAL(&s_status_lock);


    /*
     * Never invoke application code while holding the critical section.
     */
    if (callback != NULL) {
        callback(
            &status_snapshot,
            callback_user_data
        );
    }

}

static void wifi_manager_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    /*
     * These parameters will be used in later steps.
     */
    (void)handler_argument;
    (void)event_data;


    if (event_base == WIFI_EVENT) {
        switch (event_id)
        {
            case WIFI_EVENT_STA_START:
            {
                APP_LOGD(TAG, EVENT_WIFI_EVENT_STA_START_0FC585EF, "Event: WIFI_EVENT_STA_START");
                break;
            }

            case WIFI_EVENT_STA_CONNECTED:
            {
                const wifi_event_sta_connected_t *connected_event =
                    (const wifi_event_sta_connected_t *)event_data;

                taskENTER_CRITICAL(&s_status_lock);

                    s_wifi_manager.status.state =
                    WIFI_MANAGER_STATE_WAITING_FOR_IP;
                    
                    s_wifi_manager.status.disconnect_reason =
                    0U;
                    
                    s_wifi_manager.status.has_ipv4_address =
                    false;
                    
                    s_wifi_manager.status.ipv4_address[0] =
                    '\0';

                    if ((connected_event != NULL) &&
                        (connected_event->ssid_len > 0U) &&
                        (connected_event->ssid_len <= WIFI_MANAGER_SSID_MAX_LENGTH))
                    {
                        memset(
                            s_wifi_manager.status.ssid,
                            0,
                            sizeof(s_wifi_manager.status.ssid));

                        memcpy(
                            s_wifi_manager.status.ssid,
                            connected_event->ssid,
                            connected_event->ssid_len);

                        s_wifi_manager.status.ssid[connected_event->ssid_len] = '\0';
                    }

                taskEXIT_CRITICAL(&s_status_lock);

                APP_LOGI(TAG, EVENT_WIFI_EVENT_STA_CONNECT_01624329, "Event: WIFI_EVENT_STA_CONNECTED");
                APP_LOGD(TAG, WAITING_FOR_IPV4_ADDRESS_ED730DD5, "Waiting for IPv4 address");

                wifi_manager_notify_status_changed();

                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED:
                {
                    const wifi_event_sta_disconnected_t *event =
                        (const wifi_event_sta_disconnected_t *)event_data;

                    bool manual_disconnect = false;
                    bool unmanaged_disconnect = false;
                    bool stop_connection_timer = false;

                    uint16_t disconnect_reason = (uint16_t)WIFI_REASON_UNSPECIFIED;

                    if (event != NULL) {
                        disconnect_reason =
                            (uint16_t)event->reason;
                    }
                    else {
                        APP_LOGE(
                            TAG, WIFI_EVENT_STA_DISCONNECTED_20C4DC60,
                            "WIFI_EVENT_STA_DISCONNECTED has no event data"
                        );
                    }

                    taskENTER_CRITICAL(&s_status_lock);

                    s_wifi_manager.status.state =
                        WIFI_MANAGER_STATE_DISCONNECTED;

                    s_wifi_manager.status.disconnect_reason =
                        disconnect_reason;

                    s_wifi_manager.status.has_ipv4_address =
                        false;

                    s_wifi_manager.status.ipv4_address[0] =
                        '\0';

                    s_wifi_manager.status.rssi_valid =
                        false;

                    s_wifi_manager.status.rssi_dbm =
                        0;

                    manual_disconnect = s_wifi_manager.manual_disconnect_requested;
                    unmanaged_disconnect =
                        s_wifi_manager.unmanaged_disconnect_in_progress;

                    stop_connection_timer =
                        s_wifi_manager.connection_timeout_timer != NULL;

                    wifi_manager_cancel_connection_attempt_locked();
                    s_wifi_manager.connection_timeout_abort_in_progress =
                        false;
                    s_wifi_manager.unmanaged_disconnect_in_progress =
                        false;

                    taskEXIT_CRITICAL(&s_status_lock);

                    if (stop_connection_timer)
                    {
                        const esp_err_t timer_error =
                            wifi_manager_stop_connection_timeout_timer();

                        if (timer_error != ESP_OK)
                        {
                            APP_LOGD(
                                TAG, FAILED_TO_STOP_CONNECTION_TI_2C87D570,
                                "Failed to stop connection timer on "
                                "disconnect: %s",
                                esp_err_to_name(timer_error));
                        }
                    }

                    APP_LOGD(TAG, EVENT_WIFI_EVENT_STA_DISCONN_B432E25B, "Event: WIFI_EVENT_STA_DISCONNECTED");

                    wifi_manager_notify_status_changed();

                    /*
                    * Then move to RETRY_WAIT and wake the reconnect task,
                    * provided reconnect policy allows it.
                    */
                    if (manual_disconnect)
                    {
                        APP_LOGI(
                            TAG, MANUAL_WI_FI_DISCONNECT_COMP_418457C7,
                            "Manual Wi-Fi disconnect completed; "
                            "automatic reconnect suppressed");
                    }
                    else if (unmanaged_disconnect)
                    {
                        APP_LOGI(
                            TAG, UNADOPTED_PROVISIONING_CONNE_645546E2,
                            "Unadopted provisioning connection detached");
                    }
                    else if (!wifi_manager_schedule_reconnect())
                    {
                        APP_LOGD(
                            TAG, AUTOMATIC_RECONNECT_WAS_NOT_BC19F322,
                            "Automatic reconnect was not scheduled");
                    }
                }
                break;

            case WIFI_EVENT_STA_STOP:
                APP_LOGD(TAG, EVENT_WIFI_EVENT_STA_STOP_4BD830CC, "Event: WIFI_EVENT_STA_STOP");
                break;

            default:
                APP_LOGD(
                    TAG, UNHANDLED_WIFI_EVENT_ID_LD_F44CAECB,
                    "Unhandled WIFI_EVENT id=%ld",
                    (long)event_id
                );
                break;
        }

        return;
    }

    if (event_base == IP_EVENT) {
        switch (event_id)
        {
            case IP_EVENT_STA_GOT_IP:
                const ip_event_got_ip_t *got_ip_event =
                    (const ip_event_got_ip_t *)event_data;

                if (got_ip_event == NULL) {
                    APP_LOGE(
                        TAG, IP_EVENT_STA_GOT_IP_1B94B16F,
                        "IP_EVENT_STA_GOT_IP contains no event data"
                    );

                    wifi_manager_notify_status_changed();

                    break;
                }

                /*
                * Format into a local buffer before entering the critical section.
                * snprintf() should not run while interrupts/scheduling are restricted.
                */
                char ipv4_address[WIFI_MANAGER_IPV4_STRING_SIZE] = {0};

                const int written = snprintf(
                    ipv4_address,
                    sizeof(ipv4_address),
                    IPSTR,
                    IP2STR(&got_ip_event->ip_info.ip)
                );

                if ((written <= 0) ||
                    ((size_t)written >= sizeof(ipv4_address))) {

                    APP_LOGE(
                        TAG, FAILED_TO_FORMAT_STATION_IPV_4883CBD0,
                        "Failed to format Station IPv4 address"
                    );

                    break;
                }

                bool got_ip_accepted = false;
                wifi_manager_state_t state_at_event =
                    WIFI_MANAGER_STATE_UNINITIALIZED;

                taskENTER_CRITICAL(&s_status_lock);

                state_at_event =
                    s_wifi_manager.status.state;

                const bool state_accepts_got_ip =
                    (state_at_event == WIFI_MANAGER_STATE_CONNECTING) ||
                    (state_at_event == WIFI_MANAGER_STATE_WAITING_FOR_IP) ||
                    (state_at_event == WIFI_MANAGER_STATE_CONNECTED);

                /*
                 * Whichever side first owns the lock wins the GOT_IP versus
                 * timeout/manual-disconnect race. A claimed abort must finish
                 * instead of briefly publishing a false CONNECTED state.
                 * State eligibility also rejects an IP event queued before a
                 * completed disconnect boundary.
                 */
                if (state_accepts_got_ip &&
                    !s_wifi_manager.manual_disconnect_requested &&
                    !s_wifi_manager.connection_timeout_abort_in_progress &&
                    !s_wifi_manager.unmanaged_disconnect_in_progress)
                {
                    memcpy(
                        s_wifi_manager.status.ipv4_address,
                        ipv4_address,
                        sizeof(s_wifi_manager.status.ipv4_address)
                    );

                    s_wifi_manager.status.has_ipv4_address =
                        true;

                    s_wifi_manager.status.state =
                        WIFI_MANAGER_STATE_CONNECTED;

                    s_wifi_manager.reconnect_delay_ms =
                        WIFI_MANAGER_RECONNECT_INITIAL_DELAY_MS;

                    s_wifi_manager.reconnect_attempt_count =
                        0U;

                    s_wifi_manager.status.disconnect_reason =
                        0U;

                    wifi_manager_cancel_connection_attempt_locked();
                    got_ip_accepted = true;
                }

                taskEXIT_CRITICAL(&s_status_lock);

                if (!got_ip_accepted)
                {
                    APP_LOGD(
                        TAG, IGNORED_IPV4_EVENT_IN_STATE_958F8E5C,
                        "Ignored IPv4 event in state %s or while disconnect "
                        "is in progress",
                        wifi_manager_state_to_string(state_at_event));

                    break;
                }

                const esp_err_t timer_error =
                    wifi_manager_stop_connection_timeout_timer();

                if (timer_error != ESP_OK)
                {
                    APP_LOGD(
                        TAG, FAILED_TO_STOP_CONNECTION_TI_62402ADD,
                        "Failed to stop connection timer on GOT_IP: %s",
                        esp_err_to_name(timer_error));
                }

                APP_LOGI(
                    TAG, EVENT_IP_EVENT_STA_GOT_AA4AA556,
                    "Event: IP_EVENT_STA_GOT_IP, address=%s",
                    ipv4_address
                );

                wifi_manager_notify_status_changed();

                break;

            case IP_EVENT_STA_LOST_IP:
                wifi_manager_state_t resulting_state;

                taskENTER_CRITICAL(&s_status_lock);

                s_wifi_manager.status.has_ipv4_address = false;
                s_wifi_manager.status.ipv4_address[0] = '\0';

                s_wifi_manager.status.rssi_valid = false;
                s_wifi_manager.status.rssi_dbm = 0;

                /*
                * Only wait for a new IP if Wi-Fi has not already
                * transitioned to DISCONNECTED.
                */
                if ((s_wifi_manager.status.state ==
                    WIFI_MANAGER_STATE_CONNECTED) ||
                    (s_wifi_manager.status.state ==
                    WIFI_MANAGER_STATE_WAITING_FOR_IP)) {

                    s_wifi_manager.status.state =
                        WIFI_MANAGER_STATE_WAITING_FOR_IP;
                }

                resulting_state =
                    s_wifi_manager.status.state;

                taskEXIT_CRITICAL(&s_status_lock);

                APP_LOGW(
                    TAG, STATION_LOST_IPV4_ADDRESS_ST_44AD4850,
                    "Station lost IPv4 address, state=%s",
                    wifi_manager_state_to_string(resulting_state)
                );

                wifi_manager_notify_status_changed();

                break;

            default:
                APP_LOGD(
                    TAG, UNHANDLED_IP_EVENT_ID_LD_7B864352,
                    "Unhandled IP_EVENT id=%ld",
                    (long)event_id
                );
                break;
        }
    }
}

static const char *wifi_manager_rssi_to_quality(int8_t rssi)
{
    if (rssi >= -50) {
        return "EXCELLENT";
    }

    if (rssi >= -60) {
        return "GOOD";
    }

    if (rssi >= -70) {
        return "FAIR";
    }

    return "WEAK";
}

/* Functions ---------------------------------------------------------------- */
esp_err_t wifi_manager_init(void)
{
    if(s_wifi_manager.initialized == true)
    {
        APP_LOGW(TAG, WI_FI_MANAGER_IS_ALREADY_7F4A53E7, "Wi-Fi manager is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_driver_operation_mutex == NULL)
    {
        s_driver_operation_mutex =
            xSemaphoreCreateMutexStatic(
                &s_driver_operation_mutex_storage);

        if (s_driver_operation_mutex == NULL)
        {
            APP_LOGE(
                TAG, FAILED_TO_CREATE_WI_FI_C30E27D3,
                "Failed to create Wi-Fi driver operation mutex");

            return ESP_ERR_NO_MEM;
        }
    }

    /*
     * Create the default Wi-Fi Station network interface.
     *
     * Prerequisites:
     *
     *     esp_netif_init()
     *     esp_event_loop_create_default()
     *
     * Both have already been initialized by network_platform_init().
     */
    esp_netif_t *station_netif =
        esp_netif_create_default_wifi_sta();

    if (station_netif == NULL) {
        APP_LOGE(
            TAG, FAILED_TO_CREATE_DEFAULT_WI_334C329E,
            "Failed to create default Wi-Fi Station interface"
        );

        return ESP_FAIL;
    }

    TaskHandle_t reconnect_task_handle = NULL;
    esp_timer_handle_t connection_timeout_timer = NULL;

    /*
     * Initialize the Wi-Fi driver using ESP-IDF's recommended
     * default configuration.
     */
    wifi_init_config_t wifi_init_config =
        WIFI_INIT_CONFIG_DEFAULT();

    esp_err_t ret =
        esp_wifi_init(&wifi_init_config);

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_INITIALIZE_WI_FI_A4AF3509,
            "Failed to initialize Wi-Fi driver: %s",
            esp_err_to_name(ret)
        );

        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }


    /*
     * Keep driver storage volatile. Durable application credentials are owned
     * exclusively by config_manager, including provisioning handoff.
     */
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_CONFIGURE_WI_FI_B5C95D50,
            "Failed to configure Wi-Fi storage: %s",
            esp_err_to_name(ret)
        );

        const esp_err_t deinit_ret =
            esp_wifi_deinit();

        if (deinit_ret != ESP_OK) {
            APP_LOGW(
                TAG, WI_FI_CLEANUP_FAILED_S_EEBE1AED,
                "Wi-Fi cleanup failed: %s",
                esp_err_to_name(deinit_ret)
            );
        }

        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }


    /*
     * Configure the device as a Wi-Fi client.
     *
     * Station mode:
     *
     *     ESP32-S3 → connects to an existing router/access point
     */
    ret = esp_wifi_set_mode(WIFI_MODE_STA);


    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_SET_WI_FI_FA6BCDB0,
            "Failed to set Wi-Fi Station mode: %s",
            esp_err_to_name(ret)
        );

        const esp_err_t deinit_ret =
            esp_wifi_deinit();

        if (deinit_ret != ESP_OK) {
            APP_LOGW(
                TAG, WI_FI_CLEANUP_FAILED_S_EEBE1AED,
                "Wi-Fi cleanup failed: %s",
                esp_err_to_name(deinit_ret)
            );
        }

        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }

    ret = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_manager_event_handler,
        NULL,
        &s_wifi_manager.wifi_event_instance
    );

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_REGISTER_WI_FI_C2E912AB,
            "Failed to register Wi-Fi event handler: %s",
            esp_err_to_name(ret)
        );

        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }

    ret = esp_event_handler_instance_register(
        IP_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_manager_event_handler,
        NULL,
        &s_wifi_manager.ip_event_instance
    );

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_REGISTER_IP_EVENT_531220FA,
            "Failed to register IP event handler: %s",
            esp_err_to_name(ret)
        );

        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_manager.wifi_event_instance
        );

        s_wifi_manager.wifi_event_instance = NULL;

        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }

    const esp_timer_create_args_t timeout_timer_args = {
        .callback = wifi_manager_connection_timeout_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timeout",
    };

    ret = esp_timer_create(
        &timeout_timer_args,
        &connection_timeout_timer);

    if (ret != ESP_OK)
    {
        APP_LOGE(
            TAG, FAILED_TO_CREATE_WI_FI_ED459018,
            "Failed to create Wi-Fi connection timeout timer: %s",
            esp_err_to_name(ret));

        esp_event_handler_instance_unregister(
            IP_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_manager.ip_event_instance);

        s_wifi_manager.ip_event_instance = NULL;

        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_manager.wifi_event_instance);

        s_wifi_manager.wifi_event_instance = NULL;

        esp_wifi_deinit();
        esp_netif_destroy_default_wifi(station_netif);

        return ret;
    }

    BaseType_t task_result =
    xTaskCreate(
        wifi_manager_reconnect_task,
        WIFI_MANAGER_RECONNECT_TASK_NAME,
        WIFI_MANAGER_RECONNECT_TASK_STACK_SIZE,
        NULL,
        WIFI_MANAGER_RECONNECT_TASK_PRIORITY,
        &reconnect_task_handle);


    if (task_result != pdPASS)
    {
        APP_LOGE(
            TAG, FAILED_TO_CREATE_WI_FI_92F4E72E,
            "Failed to create Wi-Fi reconnect task");

        const esp_err_t timer_delete_error =
            esp_timer_delete(connection_timeout_timer);

        if (timer_delete_error != ESP_OK)
        {
            APP_LOGW(
                TAG, FAILED_TO_DELETE_CONNECTION_3359745C,
                "Failed to delete connection timer after task error: %s",
                esp_err_to_name(timer_delete_error));
        }

        connection_timeout_timer = NULL;

        esp_event_handler_instance_unregister(
            IP_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_manager.ip_event_instance);

        s_wifi_manager.ip_event_instance = NULL;

        esp_event_handler_instance_unregister(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            s_wifi_manager.wifi_event_instance);

        s_wifi_manager.wifi_event_instance = NULL;

        const esp_err_t deinit_ret =
            esp_wifi_deinit();

        if (deinit_ret != ESP_OK)
        {
            APP_LOGW(
                TAG, FAILED_TO_DEINITIALIZE_WI_FI_ACAC0F28,
                "Failed to deinitialize Wi-Fi after task error: %s",
                esp_err_to_name(deinit_ret));
        }

        esp_netif_destroy_default_wifi(
            station_netif);

        return ESP_ERR_NO_MEM;
    }

    ret = esp_wifi_start();
    
    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_START_WI_FI_FF32850F,
            "Failed to start Wi-Fi Station: %s",
            esp_err_to_name(ret)
        );
        
        if (reconnect_task_handle != NULL)
        {
            vTaskDelete(
                reconnect_task_handle);

            reconnect_task_handle = NULL;
        }

        if (connection_timeout_timer != NULL)
        {
            const esp_err_t timer_delete_error =
                esp_timer_delete(connection_timeout_timer);

            if (timer_delete_error != ESP_OK)
            {
                APP_LOGW(
                    TAG, FAILED_TO_DELETE_CONNECTION_501228E3,
                    "Failed to delete connection timer after start error: %s",
                    esp_err_to_name(timer_delete_error));
            }

            connection_timeout_timer = NULL;
        }
        /*
        * Undo IP event registration.
        */
       esp_event_handler_instance_unregister(
           IP_EVENT,
           ESP_EVENT_ANY_ID,
           s_wifi_manager.ip_event_instance
        );
        
        s_wifi_manager.ip_event_instance = NULL;
        
        /*
        * Undo Wi-Fi event registration.
        */
       esp_event_handler_instance_unregister(
           WIFI_EVENT,
           ESP_EVENT_ANY_ID,
           s_wifi_manager.wifi_event_instance
        );
        
        s_wifi_manager.wifi_event_instance = NULL;
        
        /*
        * Release Wi-Fi driver and Station interface.
        */
       const esp_err_t deinit_ret =
       esp_wifi_deinit();
       
       if (deinit_ret != ESP_OK) {
           APP_LOGW(
               TAG, FAILED_TO_DEINITIALIZE_WI_FI_5E40A3BE,
               "Failed to deinitialize Wi-Fi after start error: %s",
               esp_err_to_name(deinit_ret)
            );
        }

        esp_netif_destroy_default_wifi(station_netif);
        
        return ret;
    }
    
        /*
         * Commit component state only after all initialization steps succeed.
         */
        taskENTER_CRITICAL(
            &s_status_lock);

        s_wifi_manager.station_netif =
            station_netif;

        s_wifi_manager.reconnect_task_handle =
            reconnect_task_handle;

        s_wifi_manager.connection_timeout_timer =
            connection_timeout_timer;

        s_wifi_manager.connection_attempt_generation = 0U;
        s_wifi_manager.connection_timeout_generation = 0U;
        s_wifi_manager.connection_attempt_deadline_us = 0;
        s_wifi_manager.connection_attempt_active = false;
        s_wifi_manager.connection_timeout_pending = false;
        s_wifi_manager.connection_timeout_abort_in_progress = false;
        s_wifi_manager.unmanaged_disconnect_in_progress = false;

        s_wifi_manager.initialized =
            true;

        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_READY;

        taskEXIT_CRITICAL(
            &s_status_lock);
    
    APP_LOGI(
        TAG, WI_FI_MANAGER_INITIALIZED_MO_C11FDC03,
        "Wi-Fi manager initialized: mode=%s, storage=%s",
        "STATION",
        "RAM"
        );

    return ESP_OK;
}

esp_err_t wifi_manager_connect(
    const wifi_manager_sta_config_t *config
)
{
    ESP_RETURN_ON_FALSE(config != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Station configuration is NULL"
    );

    bool initialized = false;
    wifi_manager_state_t current_state =
        WIFI_MANAGER_STATE_UNINITIALIZED;

    taskENTER_CRITICAL(&s_status_lock);

    initialized = s_wifi_manager.initialized;
    current_state = s_wifi_manager.status.state;

    taskEXIT_CRITICAL(&s_status_lock);

    ESP_RETURN_ON_FALSE(initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    /*
     * Avoid starting another connection while one is already active.
     */
    if ((current_state ==
         WIFI_MANAGER_STATE_CONNECTING) ||
        (current_state ==
         WIFI_MANAGER_STATE_WAITING_FOR_IP) ||
        (current_state ==
         WIFI_MANAGER_STATE_CONNECTED) ||
        (current_state ==
         WIFI_MANAGER_STATE_RETRY_WAIT)) {

        APP_LOGW(
            TAG, WI_FI_CONNECTION_IS_ALREADY_5E328CC6,
            "Wi-Fi connection is already active: state=%s",
            wifi_manager_state_to_string(
                current_state
            )
        );

        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_FALSE(config->ssid != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Wi-Fi SSID is NULL"
    );

    ESP_RETURN_ON_FALSE(config->ssid[0] != '\0',
        ESP_ERR_INVALID_ARG,
        TAG,
        "Wi-Fi SSID is empty"
    );

    ESP_RETURN_ON_FALSE(config->password != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Wifi password is NULL"
    );


    const size_t ssid_length =
        strnlen(
            config->ssid,
            WIFI_MANAGER_SSID_BUFFER_SIZE
        );

    const size_t password_length =
        strnlen(
            config->password,
            WIFI_MANAGER_PASSWORD_BUFFER_SIZE
        );


    /*
     * wifi_config_t.sta.ssid has space for 32 bytes.
     * An SSID can legally use all 32 bytes.
     */
    if (ssid_length >
        WIFI_MANAGER_SSID_MAX_LENGTH) {

        APP_LOGE(
            TAG, SSID_IS_TOO_LONG_U_44161A12,
            "SSID is too long: %u bytes, maximum=%u",
            (unsigned int)ssid_length,
            (unsigned int)WIFI_MANAGER_SSID_MAX_LENGTH
        );

        return ESP_ERR_INVALID_ARG;
    }

    if (password_length >
        WIFI_MANAGER_PASSWORD_MAX_LENGTH) {

        APP_LOGE(
            TAG, PASSWORD_IS_TOO_LONG_U_3633DF05,
            "Password is too long: %u bytes, maximum=%u",
            (unsigned int)password_length,
            (unsigned int)WIFI_MANAGER_PASSWORD_MAX_LENGTH
        );

        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 3. Build ESP-IDF Station configuration.
     *
     * Zero-initialization gives sensible defaults:
     *
     * - scan all necessary channels;
     * - do not lock to a specific BSSID;
     * - use default RSSI threshold.
     */
    wifi_config_t wifi_config = {0};

    memcpy(
        wifi_config.sta.ssid,
        config->ssid,
        ssid_length
    );

    if (password_length > 0U) {
        memcpy(
            wifi_config.sta.password,
            config->password,
            password_length
        );

        /*
         * Reject deprecated/insecure APs below WPA2.
         * WPA3 is still accepted because it is stronger than WPA2.
         */
        wifi_config.sta.threshold.authmode =
            WIFI_AUTH_WPA2_PSK;
    }
    else {
        /*
         * Empty password means an open access point.
         */
        wifi_config.sta.threshold.authmode =
            WIFI_AUTH_OPEN;
    }


    /*
     * 4. Apply Station configuration before publishing ownership internally.
     * A failed driver copy must not leave reconnect/status state claiming that
     * credentials were accepted.
     */
    esp_err_t ret =
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        );

    wifi_manager_zeroize(
        &wifi_config,
        sizeof(wifi_config));

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_SET_WI_FI_E64B4AEF,
            "Failed to set Wi-Fi Station config: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * 5. Prepare internal status before calling esp_wifi_connect().
     *
     * Setting CONNECTING first avoids a race where an asynchronous
     * Wi-Fi event arrives before the state is updated.
     */
    taskENTER_CRITICAL(
        &s_status_lock);

    s_wifi_manager.credentials_configured =
        true;

    s_wifi_manager.auto_reconnect_enabled =
        true;

    s_wifi_manager.manual_disconnect_requested =
        false;

    s_wifi_manager.reconnect_delay_ms =
        WIFI_MANAGER_RECONNECT_INITIAL_DELAY_MS;

    s_wifi_manager.reconnect_attempt_count =
        0U;

    memset(
        s_wifi_manager.status.ssid,
        0,
        sizeof(s_wifi_manager.status.ssid)
    );

    memcpy(
        s_wifi_manager.status.ssid,
        config->ssid,
        ssid_length
    );


    s_wifi_manager.status.ssid[ssid_length] =
        '\0';

    s_wifi_manager.status.ipv4_address[0] =
        '\0';

    s_wifi_manager.status.has_ipv4_address =
        false;

    s_wifi_manager.status.rssi_valid =
        false;

    s_wifi_manager.status.rssi_dbm =
        0;

    s_wifi_manager.status.disconnect_reason =
        0U;

    s_wifi_manager.status.state =
        WIFI_MANAGER_STATE_CONNECTING;

    taskEXIT_CRITICAL(
        &s_status_lock);

    APP_LOGI(
        TAG, CONNECTING_TO_CONFIGURED_WI_218F9967,
        "Connecting to configured Wi-Fi network");

    /*
     * 6. Start the asynchronous connection process.
     */
    ret = wifi_manager_start_connection_attempt();

    if (ret != ESP_OK)
    {
        taskENTER_CRITICAL(
            &s_status_lock);

        s_wifi_manager.credentials_configured =
            false;

        s_wifi_manager.auto_reconnect_enabled =
            false;

        s_wifi_manager.manual_disconnect_requested =
            false;

        wifi_manager_cancel_connection_attempt_locked();

        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_FAILED;

        s_wifi_manager.status.has_ipv4_address =
            false;

        s_wifi_manager.status.ipv4_address[0] =
            '\0';

        s_wifi_manager.status.rssi_valid =
            false;

        s_wifi_manager.status.rssi_dbm =
            0;

        taskEXIT_CRITICAL(
            &s_status_lock);

        APP_LOGE(
            TAG, FAILED_TO_START_WI_FI_6BEF7973,
            "Failed to start Wi-Fi connection: %s",
            esp_err_to_name(ret));

        wifi_manager_notify_status_changed();

        return ret;
    }
    wifi_manager_notify_status_changed();

    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    ESP_RETURN_ON_FALSE(
        s_wifi_manager.initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized");

    bool timeout_abort_in_progress = false;

    /*
     * Store the application intent before calling esp_wifi_disconnect().
     *
     * WIFI_EVENT_STA_DISCONNECTED may arrive asynchronously shortly
     * after the driver API is called. The event handler must already
     * know that this was a manual disconnect.
     */
    taskENTER_CRITICAL(
        &s_status_lock);

    s_wifi_manager.manual_disconnect_requested =
        true;

    s_wifi_manager.auto_reconnect_enabled =
        false;

    s_wifi_manager.reconnect_delay_ms =
        WIFI_MANAGER_RECONNECT_INITIAL_DELAY_MS;

    s_wifi_manager.reconnect_attempt_count =
        0U;

    timeout_abort_in_progress =
        s_wifi_manager.connection_timeout_abort_in_progress;

    wifi_manager_cancel_connection_attempt_locked();

    taskEXIT_CRITICAL(
        &s_status_lock);

    const esp_err_t timer_error =
        wifi_manager_stop_connection_timeout_timer();

    if (timer_error != ESP_OK)
    {
        APP_LOGD(
            TAG, FAILED_TO_STOP_CONNECTION_TI_D9606025,
            "Failed to stop connection timer on manual disconnect: %s",
            esp_err_to_name(timer_error));
    }

    if (timeout_abort_in_progress)
    {
        APP_LOGD(
            TAG, MANUAL_DISCONNECT_JOINED_TIM_E58689D3,
            "Manual disconnect joined timeout abort already in progress");

        return ESP_OK;
    }

    const esp_err_t lock_error =
        wifi_manager_lock_driver_operation();

    if (lock_error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_status_lock);
        s_wifi_manager.status.state = WIFI_MANAGER_STATE_FAILED;
        taskEXIT_CRITICAL(&s_status_lock);

        wifi_manager_notify_status_changed();
        return lock_error;
    }

    APP_LOGI(
        TAG, MANUAL_WI_FI_DRIVER_DETACH_A05B90D8,
        "Manual Wi-Fi driver detach requested");

    esp_err_t error =
        esp_wifi_disconnect();

    wifi_manager_unlock_driver_operation();

    if (error == ESP_ERR_WIFI_NOT_CONNECT)
    {
        /*
         * No DISCONNECTED event is guaranteed when association disappeared
         * just before this call. Complete the quiescent state locally so a
         * factory-reset preparation cannot wait on an event that will never
         * arrive.
         */
        taskENTER_CRITICAL(
            &s_status_lock);

        s_wifi_manager.connection_timeout_abort_in_progress = false;
        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_DISCONNECTED;
        s_wifi_manager.status.has_ipv4_address = false;
        s_wifi_manager.status.ipv4_address[0] = '\0';
        s_wifi_manager.status.rssi_valid = false;
        s_wifi_manager.status.rssi_dbm = 0;

        taskEXIT_CRITICAL(
            &s_status_lock);

        APP_LOGI(
            TAG, WI_FI_DRIVER_WAS_ALREADY_95551484,
            "Wi-Fi driver was already detached; reconnect remains disabled");

        wifi_manager_notify_status_changed();

        return ESP_OK;
    }

    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(
            &s_status_lock);

        /*
         * Preserve the manual-disconnect policy even when the driver
         * call fails. The application explicitly requested that the
         * device remain offline.
         */
        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_FAILED;

        taskEXIT_CRITICAL(
            &s_status_lock);

        APP_LOGE(
            TAG, FAILED_TO_DISCONNECT_WI_FI_CD2149E7,
            "Failed to disconnect Wi-Fi: %s",
            esp_err_to_name(error));

        wifi_manager_notify_status_changed();

        return error;
    }

    /*
     * WIFI_EVENT_STA_DISCONNECTED will finish the asynchronous
     * status transition.
     */
    return ESP_OK;
}

esp_err_t wifi_manager_discard_unmanaged_connection(void)
{
    bool initialized = false;
    bool owned_connection = false;
    bool discard_already_in_progress = false;

    taskENTER_CRITICAL(&s_status_lock);

    initialized =
        s_wifi_manager.initialized;

    owned_connection =
        s_wifi_manager.credentials_configured ||
        s_wifi_manager.auto_reconnect_enabled ||
        s_wifi_manager.manual_disconnect_requested ||
        s_wifi_manager.connection_timeout_abort_in_progress;

    discard_already_in_progress =
        s_wifi_manager.unmanaged_disconnect_in_progress;

    if (initialized &&
        !owned_connection &&
        !discard_already_in_progress)
    {
        /*
         * Claim the cleanup boundary before touching the driver. A queued
         * GOT_IP event must not publish CONNECTED after provisioning has
         * already concluded that no verified credential handoff exists.
         */
        s_wifi_manager.unmanaged_disconnect_in_progress =
            true;

        wifi_manager_cancel_connection_attempt_locked();
    }

    taskEXIT_CRITICAL(&s_status_lock);

    if (!initialized || owned_connection)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (discard_already_in_progress)
    {
        return ESP_OK;
    }

    const esp_err_t timer_error =
        wifi_manager_stop_connection_timeout_timer();

    if (timer_error != ESP_OK)
    {
        APP_LOGD(
            TAG, FAILED_TO_STOP_STALE_CONNECT_3B9A3A65,
            "Failed to stop stale connection timer during provisioning "
            "cleanup: %s",
            esp_err_to_name(timer_error));
    }

    /*
     * network_provisioning selects WIFI_STORAGE_FLASH while it validates
     * credentials. The framework is STOPPED before this API is called, so
     * restore wifi_manager's RAM-only runtime policy before another session
     * can begin. Persistent driver cleanup remains a separate factory-reset
     * operation.
     */
    const esp_err_t lock_error =
        wifi_manager_lock_driver_operation();

    if (lock_error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_status_lock);
        s_wifi_manager.unmanaged_disconnect_in_progress = false;
        s_wifi_manager.status.state = WIFI_MANAGER_STATE_FAILED;
        taskEXIT_CRITICAL(&s_status_lock);

        wifi_manager_notify_status_changed();
        return lock_error;
    }

    const esp_err_t storage_error =
        esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (storage_error != ESP_OK)
    {
        wifi_manager_unlock_driver_operation();

        taskENTER_CRITICAL(&s_status_lock);

        s_wifi_manager.unmanaged_disconnect_in_progress = false;
        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_FAILED;

        taskEXIT_CRITICAL(&s_status_lock);

        APP_LOGE(
            TAG, FAILED_TO_RESTORE_WI_FI_BCCE9EB2,
            "Failed to restore Wi-Fi RAM storage during provisioning "
            "cleanup: %s",
            esp_err_to_name(storage_error));

        wifi_manager_notify_status_changed();

        return storage_error;
    }

    APP_LOGI(
        TAG, DISCARDING_UNADOPTED_PROVISI_599EC97F,
        "Discarding unadopted provisioning connection");

    const esp_err_t error =
        esp_wifi_disconnect();

    wifi_manager_unlock_driver_operation();

    if (error == ESP_ERR_WIFI_NOT_CONNECT)
    {
        /* No asynchronous event is guaranteed when the driver is offline. */
        taskENTER_CRITICAL(&s_status_lock);

        s_wifi_manager.unmanaged_disconnect_in_progress =
            false;

        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_DISCONNECTED;

        s_wifi_manager.status.has_ipv4_address =
            false;

        s_wifi_manager.status.ipv4_address[0] =
            '\0';

        s_wifi_manager.status.rssi_valid =
            false;

        s_wifi_manager.status.rssi_dbm =
            0;

        taskEXIT_CRITICAL(&s_status_lock);

        wifi_manager_notify_status_changed();

        APP_LOGD(
            TAG, NO_ACTIVE_UNADOPTED_STATION_91C30067,
            "No active unadopted Station connection remained");

        return ESP_OK;
    }

    if (error != ESP_OK)
    {
        taskENTER_CRITICAL(&s_status_lock);

        s_wifi_manager.unmanaged_disconnect_in_progress =
            false;

        s_wifi_manager.status.state =
            WIFI_MANAGER_STATE_FAILED;

        taskEXIT_CRITICAL(&s_status_lock);

        APP_LOGE(
            TAG, FAILED_TO_DISCARD_UNADOPTED_4A11334D,
            "Failed to discard unadopted Station connection: %s",
            esp_err_to_name(error));

        wifi_manager_notify_status_changed();

        return error;
    }

    /* WIFI_EVENT_STA_DISCONNECTED completes the transient operation. */
    return ESP_OK;
}

esp_err_t wifi_manager_clear_persistent_driver_settings(void)
{
    taskENTER_CRITICAL(&s_status_lock);

    const bool initialized =
        s_wifi_manager.initialized;

    taskEXIT_CRITICAL(&s_status_lock);

    if (!initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * network_provisioning writes received credentials using
     * WIFI_STORAGE_FLASH. Keep this driver-owned cleanup behind wifi_manager
     * instead of exposing esp_wifi_restore() to application coordinators.
     * Reboot promptly after the complete reset transaction succeeds because
     * restore resets persistent Wi-Fi mode, protocol, bandwidth, and Station
     * configuration.
     */
    const esp_err_t lock_error =
        wifi_manager_lock_driver_operation();

    if (lock_error != ESP_OK)
    {
        return lock_error;
    }

    const esp_err_t error =
        esp_wifi_restore();

    wifi_manager_unlock_driver_operation();

    if (error != ESP_OK)
    {
        APP_LOGE(
            TAG, FAILED_TO_CLEAR_PERSISTENT_W_41FCC122,
            "Failed to clear persistent Wi-Fi driver settings: %s",
            esp_err_to_name(error));

        return error;
    }

    APP_LOGI(
        TAG, PERSISTENT_WI_FI_DRIVER_SETT_F68378EE,
        "Persistent Wi-Fi driver settings cleared");

    return ESP_OK;
}

esp_err_t wifi_manager_get_status(
    wifi_manager_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "Output status pointer is NULL"
    );

    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    taskENTER_CRITICAL(&s_status_lock);

    memcpy(
        status,
        &s_wifi_manager.status,
        sizeof(*status)
    );

    taskEXIT_CRITICAL(&s_status_lock);

    return ESP_OK;
}

esp_err_t wifi_manager_get_rssi(
    int8_t *rssi_dbm)
{
    ESP_RETURN_ON_FALSE(rssi_dbm != NULL, 
        ESP_ERR_INVALID_ARG,
        TAG,
        "RSSI output pointer is NULL"
    );

    ESP_RETURN_ON_FALSE(s_wifi_manager.initialized == true, 
        ESP_ERR_INVALID_STATE,
        TAG,
        "Wi-Fi manager is not initialized"
    );

    /*
     * Check state quickly under the status lock.
     */
    bool connected = false;

    taskENTER_CRITICAL(&s_status_lock);

    connected =
        (s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_CONNECTED) &&
        s_wifi_manager.status.has_ipv4_address;

    taskEXIT_CRITICAL(&s_status_lock);

    if (!connected) {
        APP_LOGW(TAG, CANNOT_READ_RSSI_WHILE_WI_296220EA, "Cannot read RSSI while Wi-Fi is disconnected");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Do not call ESP Wi-Fi APIs inside a critical section.
     */
    int current_rssi = 0;

    const esp_err_t ret =
        esp_wifi_sta_get_rssi(&current_rssi);

    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);

        s_wifi_manager.status.rssi_valid = false;
        s_wifi_manager.status.rssi_dbm = 0;

        taskEXIT_CRITICAL(&s_status_lock);

        APP_LOGE(
            TAG, FAILED_TO_READ_WI_FI_AED4CB08,
            "Failed to read Wi-Fi RSSI: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    const int8_t rssi_value =
        (int8_t)current_rssi;

    taskENTER_CRITICAL(&s_status_lock);

    s_wifi_manager.status.rssi_dbm =
        rssi_value;

    s_wifi_manager.status.rssi_valid =
        true;

    taskEXIT_CRITICAL(&s_status_lock);

    *rssi_dbm = rssi_value;

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    bool connected = false;

    taskENTER_CRITICAL(&s_status_lock);

    connected =
        s_wifi_manager.initialized &&
        s_wifi_manager.credentials_configured &&
        (s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_CONNECTED) &&
        s_wifi_manager.status.has_ipv4_address;

    taskEXIT_CRITICAL(&s_status_lock);

    return connected;
}

esp_err_t wifi_manager_register_status_callback(
    wifi_manager_status_callback_t callback,
    void *user_data
)
{

    taskENTER_CRITICAL(&s_status_lock);

    s_wifi_manager.status_callback = callback;
    s_wifi_manager.status_callback_user_data = user_data;

    taskEXIT_CRITICAL(&s_status_lock);

    APP_LOGD(
        TAG, STATUS_CALLBACK_S_1ECE06DB,
        "Status callback %s",
        callback != NULL
            ? "registered"
            : "unregistered"
    );

    /*
     * A newly registered consumer may have missed startup or provisioning
     * events. Deliver the current snapshot immediately. The helper copies the
     * state and invokes application code outside the critical section.
     */
    if (callback != NULL)
    {
        wifi_manager_notify_status_changed();
    }

    return ESP_OK;
}

const char *wifi_manager_state_to_string(
    wifi_manager_state_t state)
{
    switch (state)
    {
        case WIFI_MANAGER_STATE_UNINITIALIZED:
            return "UNINITIALIZED";

        case WIFI_MANAGER_STATE_READY:
            return "READY";

        case WIFI_MANAGER_STATE_CONNECTING:
            return "CONNECTING";

        case WIFI_MANAGER_STATE_WAITING_FOR_IP:
            return "WAITING_FOR_IP";

        case WIFI_MANAGER_STATE_CONNECTED:
            return "CONNECTED";

        case WIFI_MANAGER_STATE_DISCONNECTED:
            return "DISCONNECTED";

        case WIFI_MANAGER_STATE_FAILED:
            return "FAILED";

        case WIFI_MANAGER_STATE_RETRY_WAIT:
            return "RETRY_WAIT";

        default:
            return "UNKNOWN";
    }
}

esp_err_t wifi_manager_scan_and_log(void)
{
    if (!s_wifi_manager.initialized) {
        APP_LOGE(
            TAG, CANNOT_SCAN_BECAUSE_WI_FI_39AB8F48,
            "Cannot scan because Wi-Fi manager is not initialized"
        );

        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Scan configuration:
     *
     * ssid        = NULL : do not filter by SSID
     * bssid       = NULL : do not filter by BSSID
     * channel     = 0    : scan all supported channels
     * show_hidden = true : include APs with hidden SSIDs
     * scan_type   = active scan
     */
    const wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0U,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,

        .scan_time = {
            .active = {
                .min = 0U,
                .max = 120U,
            },
        },

        /*
         * While connected, periodically return to the current AP's
         * channel so normal Wi-Fi traffic still has an opportunity
         * to run.
         */
        .home_chan_dwell_time = 30U,
    };

    APP_LOGI(TAG, STARTING_ALL_CHANNEL_WI_FI_EB638034, "Starting all-channel Wi-Fi scan");

    /*
     * block = true:
     *
     * This task waits here until the scan finishes.
     *
     * A blocked scan does not generate WIFI_EVENT_SCAN_DONE.
     */
    esp_err_t ret =
        esp_wifi_scan_start(
            &scan_config,
            true
        );

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_START_WI_FI_37BAAEED,
            "Failed to start Wi-Fi scan: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    uint16_t ap_count = 0U;

    ret = esp_wifi_scan_get_ap_num(&ap_count);

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_OBTAIN_SCANNED_AP_CA3E47AA,
            "Failed to obtain scanned AP count: %s",
            esp_err_to_name(ret)
        );

        /*
         * Release result memory owned by the Wi-Fi driver.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            APP_LOGW(
                TAG, FAILED_TO_CLEAR_WI_FI_2AF84F3F,
                "Failed to clear Wi-Fi scan list: %s",
                esp_err_to_name(clear_ret)
            );
        }

        return ret;
    }

    APP_LOGI(
        TAG, WI_FI_SCAN_COMPLETED_FOUND_350B8AA8,
        "Wi-Fi scan completed: found=%u AP records",
        (unsigned int)ap_count
    );

    if (ap_count == 0U) {
        /*
         * No records will be fetched, so explicitly clear the list.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            APP_LOGW(
                TAG, FAILED_TO_CLEAR_EMPTY_WI_4EC0C208,
                "Failed to clear empty Wi-Fi scan list: %s",
                esp_err_to_name(clear_ret)
            );

            return clear_ret;
        }

        return ESP_OK;
    }

    wifi_ap_record_t *ap_records =
        calloc(
            ap_count,
            sizeof(*ap_records)
        );

    if (ap_records == NULL) {
        APP_LOGE(
            TAG, NO_MEMORY_FOR_U_WI_24BA34F5,
            "No memory for %u Wi-Fi AP records",
            (unsigned int)ap_count
        );

        /*
         * The scan-result list is still owned by the Wi-Fi driver.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            APP_LOGW(
                TAG, FAILED_TO_CLEAR_SCAN_LIST_9262D235,
                "Failed to clear scan list after allocation error: %s",
                esp_err_to_name(clear_ret)
            );
        }

        return ESP_ERR_NO_MEM;
    }

    /*
     * Input:
     *     records_to_read is the capacity of ap_records.
     *
     * Output:
     *     records_to_read becomes the number of records returned.
     */
    uint16_t records_to_read = ap_count;

    ret = esp_wifi_scan_get_ap_records(
        &records_to_read,
        ap_records
    );

    if (ret != ESP_OK) {
        APP_LOGE(
            TAG, FAILED_TO_RETRIEVE_WI_FI_90920A38,
            "Failed to retrieve Wi-Fi AP records: %s",
            esp_err_to_name(ret)
        );

        /*
         * Be defensive in case the driver still owns scan entries.
         */
        const esp_err_t clear_ret =
            esp_wifi_clear_ap_list();

        if (clear_ret != ESP_OK) {
            APP_LOGW(
                TAG, FAILED_TO_CLEAR_SCAN_LIST_8C17B728,
                "Failed to clear scan list after retrieval error: %s",
                esp_err_to_name(clear_ret)
            );
        }

        free(ap_records);

        return ret;
    }

    APP_LOGD(
        TAG, MESSAGE_10224D18,
        "------------------------------------------------------------"
    );

    APP_LOGD(
        TAG, NO_RSSI_CHANNEL_QUALITY_0BFA714F,
        " No. | RSSI | Channel | Quality"
    );

    APP_LOGD(
        TAG, MESSAGE_10224D18,
        "------------------------------------------------------------"
    );

    for (uint16_t index = 0U;
         index < records_to_read;
         ++index) {

        const wifi_ap_record_t *record =
            &ap_records[index];

        APP_LOGD(
            TAG, U_D_U_S_FED18C4E,
            "%4u | %4d | %7u | %-9s",
            (unsigned int)(index + 1U),
            (int)record->rssi,
            (unsigned int)record->primary,
            wifi_manager_rssi_to_quality(record->rssi)
        );
    }

    APP_LOGD(
        TAG, MESSAGE_10224D18,
        "------------------------------------------------------------"
    );

    free(ap_records);

    return ESP_OK;
}

esp_err_t wifi_manager_adopt_active_connection(void)
{
    bool initialized = false;

    taskENTER_CRITICAL(&s_status_lock);

    initialized =
        s_wifi_manager.initialized;

    taskEXIT_CRITICAL(&s_status_lock);

    if (!initialized)
    {
        APP_LOGE(
            TAG, CANNOT_ADOPT_CONNECTION_BECA_F61698B2,
            "Cannot adopt connection because Wi-Fi manager is not initialized");

        return ESP_ERR_INVALID_STATE;
    }

    /*
     * network_provisioning changes Wi-Fi storage to FLASH.
     * Restore the runtime policy owned by wifi_manager.
     *
     * This does not disconnect the current Station connection.
     */
    esp_err_t ret =
        esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (ret != ESP_OK)
    {
        APP_LOGE(
            TAG, FAILED_TO_RESTORE_WI_FI_B8C36A44,
            "Failed to restore Wi-Fi RAM storage: %s",
            esp_err_to_name(ret));

        return ret;
    }

    /*
     * There should be no wifi_manager-owned timeout for a connection
     * established by provisioning, but stop any stale timer defensively.
     */
    ret =
        wifi_manager_stop_connection_timeout_timer();

    if (ret != ESP_OK)
    {
        APP_LOGE(
            TAG, FAILED_TO_STOP_CONNECTION_TI_C57F2057,
            "Failed to stop connection timeout during adoption: %s",
            esp_err_to_name(ret));

        return ret;
    }

    bool adopted = false;

    taskENTER_CRITICAL(&s_status_lock);

    const bool can_adopt =
        s_wifi_manager.initialized &&
        (s_wifi_manager.status.state ==
         WIFI_MANAGER_STATE_CONNECTED) &&
        s_wifi_manager.status.has_ipv4_address &&
        (s_wifi_manager.status.ssid[0] != '\0') &&
        !s_wifi_manager.manual_disconnect_requested &&
        !s_wifi_manager.connection_timeout_abort_in_progress &&
        !s_wifi_manager.unmanaged_disconnect_in_progress &&
        (s_wifi_manager.reconnect_task_handle != NULL) &&
        (s_wifi_manager.connection_timeout_timer != NULL);

    if (can_adopt)
    {
        s_wifi_manager.credentials_configured =
            true;

        s_wifi_manager.auto_reconnect_enabled =
            true;

        s_wifi_manager.manual_disconnect_requested =
            false;

        s_wifi_manager.reconnect_delay_ms =
            WIFI_MANAGER_RECONNECT_INITIAL_DELAY_MS;

        s_wifi_manager.reconnect_attempt_count =
            0U;

        wifi_manager_cancel_connection_attempt_locked();

        adopted = true;
    }

    taskEXIT_CRITICAL(&s_status_lock);

    if (!adopted)
    {
        APP_LOGW(
            TAG, ACTIVE_WI_FI_CONNECTION_IS_8CA09B58,
            "Active Wi-Fi connection is not eligible for adoption");

        return ESP_ERR_INVALID_STATE;
    }

    APP_LOGI(
        TAG, ACTIVE_PROVISIONING_CONNECTI_B9EEB502,
        "Active provisioning connection adopted by Wi-Fi manager");

    /*
     * Provisioning can establish the connection before an application
     * consumer observes its Wi-Fi/IP events. Republish the adopted CONNECTED
     * snapshot so the GUI and other consumers converge immediately.
     */
    wifi_manager_notify_status_changed();

    return ESP_OK;
}
