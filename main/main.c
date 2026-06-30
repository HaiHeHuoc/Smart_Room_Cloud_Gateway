#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_common.h"
#include "board_config.h"



/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
const char *TAG = "Main_App";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */

/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */

/* Application ------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "PROJECT: %s", APP_PROJECT_NAME);
    ESP_LOGI(TAG, "VERSION: %s", APP_PROJECT_VER);
    ESP_LOGI(TAG, "BUILD DATE: %s", APP_PROJECT_VER_DATE);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    
}

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
