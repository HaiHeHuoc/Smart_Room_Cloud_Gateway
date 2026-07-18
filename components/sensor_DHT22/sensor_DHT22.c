/* Includes ----------------------------------------------------------------- */
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "dht.h"

#include "board_config.h"
#include "sensor_DHT22.h"

/* Macros ------------------------------------------------------------------- */
#define DHT22_BRINGUP_TASK_NAME         "DHT22 task test"
#define DHT22_BRINGUP_TASK_STACK_SIZE   2048
#define DHT22_BRINGUP_TASK_PRIORITY     5
#define DHT22_SAMPLE_PERIOD_MS          2500
#define DHT22_INITIAL_DELAY_MS          2000

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "DHT22 Sensor";

/* Function Prototypes ------------------------------------------------------ */
static void dht22_bringup_task(void *argument);

/* Static Functions --------------------------------------------------------- */
static void dht22_bringup_task(void *argument)
{
    (void)argument;

    /* Give the sensor time to stabilize after power-up. */
    vTaskDelay(pdMS_TO_TICKS(DHT22_INITIAL_DELAY_MS));

    TickType_t last_wake_time = xTaskGetTickCount();

    while (true)
    {

        dht22_sensor_data_t data;

        esp_err_t err = dht22_sensor_read(&data);

        if (err != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "DHT22 read failed: %s",
                esp_err_to_name(err));
        }
        else if (!isfinite(data.temperature_c) ||
                 !isfinite(data.humidity_percent) ||
                 data.temperature_c < -40.0f ||
                 data.temperature_c > 80.0f ||
                 data.humidity_percent < 0.0f ||
                 data.humidity_percent > 100.0f)
        {
            /* Reject values outside the operating range stated by DHT22. */
            ESP_LOGW(
                TAG,
                "Invalid data: temperature=%.1f C, humidity=%.1f %%",
                data.temperature_c,
                data.humidity_percent);
        }
        else
        {
            ESP_LOGD(
                TAG,
                "Temperature: %.1f C | Humidity: %.1f %%",
                data.temperature_c,
                data.humidity_percent);
        }

        /* Keep a stable sample interval without accumulating task drift. */
        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(DHT22_SAMPLE_PERIOD_MS));
    }
}

/* Functions ---------------------------------------------------------------- */
void dht22_bringup_start(void)
{
    /* Start the standalone periodic sensor logging task used for bring-up. */
    xTaskCreate(
        &dht22_bringup_task,
        DHT22_BRINGUP_TASK_NAME,
        DHT22_BRINGUP_TASK_STACK_SIZE,
        NULL,
        DHT22_BRINGUP_TASK_PRIORITY,
        NULL
    );
}

esp_err_t dht22_sensor_read(
    dht22_sensor_data_t *data)
{
    ESP_RETURN_ON_FALSE(
        data != NULL,
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid output variable"
    );

    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;

    /* The DHT driver writes humidity first and temperature second. */
    ESP_RETURN_ON_ERROR(
        dht_read_float_data(
            DHT_TYPE_AM2301,
            DHT22_PIN_GPIO,
            &humidity_percent,
            &temperature_c
        ),
        TAG,
        "Error when retrieving temperature and humidity data"
    );

    if (!isfinite(temperature_c) ||
        !isfinite(humidity_percent) ||
        temperature_c < -40.0f ||
        temperature_c > 80.0f ||
        humidity_percent < 0.0f ||
        humidity_percent > 100.0f)
    {
        ESP_LOGW(
            TAG,
            "Invalid data: temperature=%.1f C, humidity=%.1f %%",
            temperature_c,
            humidity_percent);

        return ESP_ERR_INVALID_RESPONSE;
    }

    data->humidity_percent = humidity_percent;
    data->temperature_c = temperature_c;

    return ESP_OK;
}
