#pragma once

#include "esp_err.h"

/**
 * @brief Start the ESP32-S3 performance monitor task.
 *
 * The task periodically reports:
 * - Five-second CPU average, 500 ms-sampled CPU peak, and idle percentage
 * - Internal RAM
 * - PSRAM
 * - DMA-capable RAM
 * - Running application partition usage
 * - Monitor task stack high-water mark
 *
 * @return
 *      - ESP_OK if the task was created
 *      - ESP_ERR_INVALID_STATE if already started
 *      - ESP_ERR_NOT_SUPPORTED if runtime statistics are disabled
 *      - ESP_ERR_NO_MEM if task creation failed
 */
esp_err_t performance_monitor_start(void);
