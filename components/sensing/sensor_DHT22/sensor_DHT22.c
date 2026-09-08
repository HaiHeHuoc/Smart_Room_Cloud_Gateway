/* Includes ----------------------------------------------------------------- */
#include "sensor_DHT22.h"

#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "app_log.h"

#include "dht.h"

/* Macros ------------------------------------------------------------------- */
#define DHT22_BRINGUP_TASK_NAME         "DHT22 task test"
#define DHT22_BRINGUP_TASK_STACK_SIZE   2048
#define DHT22_BRINGUP_TASK_PRIORITY     5
#define DHT22_SAMPLE_PERIOD_MS          2000
#define DHT22_INITIAL_DELAY_MS          2000

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "DHT22 Sensor";

/* Static Variables --------------------------------------------------------- */
static gpio_num_t s_gpio_num;
static bool s_is_initialized;

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
            APP_LOGE(
                TAG, DHT22_READ_FAILED_S_996CEBE9,
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
            APP_LOGW(
                TAG, INVALID_DATA_TEMPERATURE_F_C_CAF8D566,
                "Invalid data: temperature=%.1f C, humidity=%.1f %%",
                data.temperature_c,
                data.humidity_percent);
        }
        else
        {
            APP_LOGD(
                TAG, TEMPERATURE_F_C_HUMIDITY_F_24D08329,
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
esp_err_t dht22_sensor_init(
    const dht22_sensor_config_t *config)
{
    if ((config == NULL) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->gpio_num))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_is_initialized)
    {
        return (s_gpio_num == config->gpio_num)
                   ? ESP_OK
                   : ESP_ERR_INVALID_STATE;
    }

    s_gpio_num = config->gpio_num;
    s_is_initialized = true;

    APP_LOGI(
        TAG, DHT22_SENSOR_INITIALIZED,
        "gpio=%d",
        (int)s_gpio_num);

    return ESP_OK;
}

void dht22_bringup_start(void)
{
    if (!s_is_initialized)
    {
        APP_LOGE(
            TAG, DHT22_BRINGUP_BEFORE_INIT,
            "DHT22 bring-up requested before initialization");
        return;
    }

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

    ESP_RETURN_ON_FALSE(
        s_is_initialized,
        ESP_ERR_INVALID_STATE,
        TAG,
        "DHT22 sensor is not initialized"
    );

    float temperature_c = 0.0f;
    float humidity_percent = 0.0f;

    /* The DHT driver writes humidity first and temperature second. */
    ESP_RETURN_ON_ERROR(
        dht_read_float_data(
            DHT_TYPE_AM2301,
            s_gpio_num,
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
        APP_LOGW(
            TAG, INVALID_DATA_TEMPERATURE_F_C_2A248DFD,
            "Invalid data: temperature=%.1f C, humidity=%.1f %%",
            temperature_c,
            humidity_percent);

        return ESP_ERR_INVALID_RESPONSE;
    }

    data->humidity_percent = humidity_percent;
    data->temperature_c = temperature_c;

    return ESP_OK;
}
