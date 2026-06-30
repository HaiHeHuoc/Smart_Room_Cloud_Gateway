#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

const char *TAG = "Main_Function";

/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */
uint64_t gCounter = 0;

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */

/* Application ------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Smart Room Cloud Gateway");
    ESP_LOGI(TAG, "Sprint 0 - Project setup");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    while (1)
    {
        gCounter++;
        ESP_LOGI(TAG, "Counter: %llu", gCounter);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    
}

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */