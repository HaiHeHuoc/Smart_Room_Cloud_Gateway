#include "sensor_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_err.h"

#include "sensor_DHT22.h"

#define SENSOR_MANAGER_TASK_NAME "sensor_manager"
#define SENSOR_MANAGER_TASK_STACK_SIZE         3072U
#define SENSOR_MANAGER_TASK_PRIORITY           4U

#define SENSOR_MANAGER_INITIAL_DELAY_MS        2000U
#define SENSOR_MANAGER_MIN_SAMPLE_PERIOD_MS    2000U
#define SENSOR_MANAGER_MUTEX_TIMEOUT_MS        100U

const char* TAG = "SENSOR_MANAGER";

static sensor_manager_config_t s_config;
static sensor_manager_status_t s_status;

static SemaphoreHandle_t s_status_mutex;
static TaskHandle_t s_task_handle;

static bool s_is_initialized;
static bool s_is_running;

static int64_t sensor_manager_get_time_ms(void);

static bool sensor_manager_is_data_stale_locked(
    int64_t current_time_ms);

static void sensor_manager_update_success(
    const dht22_sensor_data_t *data);

static void sensor_manager_update_failure(
    esp_err_t error);

static void sensor_manager_task(
    void *argument);

static int64_t sensor_manager_get_time_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool sensor_manager_is_data_stale_locked(
    int64_t current_time_ms)
{
    if (!s_status.data_valid ||
        s_status.last_success_time_ms <= 0)
    {
        return true;
    }

    int64_t data_age_ms =
        current_time_ms - s_status.last_success_time_ms;

    return data_age_ms >=
           (int64_t)s_config.stale_timeout_ms;
}

static void sensor_manager_update_success(
    const dht22_sensor_data_t *data)
{
    if (data == NULL)
    {
        return;
    }

    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.temperature_c =
        data->temperature_c;

    s_status.humidity_percent =
        data->humidity_percent;

    s_status.data_valid = true;
    s_status.data_stale = false;

    s_status.last_error = ESP_OK;

    s_status.successful_read_count++;
    s_status.consecutive_failure_count = 0U;

    s_status.last_success_time_ms =
        sensor_manager_get_time_ms();

    s_status.state =
        SENSOR_MANAGER_STATE_READY;

    xSemaphoreGive(s_status_mutex);
}

static void sensor_manager_update_failure(
    esp_err_t error)
{
    int64_t current_time_ms =
        sensor_manager_get_time_ms();

    if (xSemaphoreTake(
            s_status_mutex,
            portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    s_status.last_error = error;

    s_status.failed_read_count++;
    s_status.consecutive_failure_count++;

    s_status.data_stale =
        sensor_manager_is_data_stale_locked(
            current_time_ms);

    /*
     * Do not overwrite temperature or humidity.
     * Preserve the last-known-good sample.
     */
    if (!s_status.data_valid ||
        s_status.data_stale)
    {
        s_status.state =
            SENSOR_MANAGER_STATE_ERROR;
    }
    else
    {
        s_status.state =
            SENSOR_MANAGER_STATE_DEGRADED;
    }

    xSemaphoreGive(s_status_mutex);
}


static void sensor_manager_task(
    void *argument)
{
    (void)argument;

    /*
     * Allow DHT22 to stabilize after power-up.
     */
    vTaskDelay(
        pdMS_TO_TICKS(
            SENSOR_MANAGER_INITIAL_DELAY_MS));

    TickType_t last_wake_time =
        xTaskGetTickCount();

    TickType_t sample_period_ticks =
        pdMS_TO_TICKS(
            s_config.sample_period_ms);

    while (true)
    {
        dht22_sensor_data_t data = {0};

        esp_err_t error =
            dht22_sensor_read(&data);

        if (error == ESP_OK)
        {
            sensor_manager_update_success(&data);

            ESP_LOGD(
                TAG,
                "Temperature: %.1f C | Humidity: %.1f %%",
                data.temperature_c,
                data.humidity_percent);
        }
        else
        {
            sensor_manager_update_failure(error);

            ESP_LOGW(
                TAG,
                "DHT22 read failed: %s",
                esp_err_to_name(error));
        }

        /*
         * Keep a stable sample schedule without accumulating
         * the sensor-read execution time as drift.
         */
        vTaskDelayUntil(
            &last_wake_time,
            sample_period_ticks);
    }
}

esp_err_t sensor_manager_start(void)
{
    if (!s_is_initialized ||
        s_is_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_status_mutex,
            pdMS_TO_TICKS(
                SENSOR_MANAGER_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    s_status.state =
        SENSOR_MANAGER_STATE_RUNNING;

    xSemaphoreGive(s_status_mutex);

    s_is_running = true;

    BaseType_t result =
        xTaskCreate(
            sensor_manager_task,
            SENSOR_MANAGER_TASK_NAME,
            SENSOR_MANAGER_TASK_STACK_SIZE,
            NULL,
            SENSOR_MANAGER_TASK_PRIORITY,
            &s_task_handle);

    if (result != pdPASS)
    {
        s_is_running = false;
        s_task_handle = NULL;

        if (xSemaphoreTake(
                s_status_mutex,
                pdMS_TO_TICKS(
                    SENSOR_MANAGER_MUTEX_TIMEOUT_MS)) == pdTRUE)
        {
            s_status.state =
                SENSOR_MANAGER_STATE_INITIALIZED;

            xSemaphoreGive(s_status_mutex);
        }

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Started");

    return ESP_OK;
}


esp_err_t sensor_manager_init(
    const sensor_manager_config_t *config)
{
    if (config == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (config->sample_period_ms <
        SENSOR_MANAGER_MIN_SAMPLE_PERIOD_MS)
    {
        ESP_LOGE(
            TAG,
            "Sample period must be at least %u ms",
            SENSOR_MANAGER_MIN_SAMPLE_PERIOD_MS);

        return ESP_ERR_INVALID_ARG;
    }

    if (config->stale_timeout_ms <=
        config->sample_period_ms)
    {
        ESP_LOGE(
            TAG,
            "Stale timeout must be greater than sample period");

        return ESP_ERR_INVALID_ARG;
    }

    s_status_mutex =
        xSemaphoreCreateMutex();

    if (s_status_mutex == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_config = *config;

    memset(
        &s_status,
        0,
        sizeof(s_status));

    s_status.state =
        SENSOR_MANAGER_STATE_INITIALIZED;

    s_status.data_valid = false;
    s_status.data_stale = true;
    s_status.last_error = ESP_OK;

    s_is_initialized = true;

    ESP_LOGI(
        TAG,
        "Initialized: period=%lu ms, stale=%lu ms",
        (unsigned long)s_config.sample_period_ms,
        (unsigned long)s_config.stale_timeout_ms);

    return ESP_OK;
}


esp_err_t sensor_manager_get_status(
    sensor_manager_status_t *status)
{
    if (status == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_is_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(
            s_status_mutex,
            pdMS_TO_TICKS(
                SENSOR_MANAGER_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    *status = s_status;

    xSemaphoreGive(s_status_mutex);

    return ESP_OK;
}

