#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool started;
    bool sample_valid;
    uint32_t report_index;
    int64_t captured_at_us;
    uint32_t cpu_used_x10;
    uint32_t cpu_peak_500ms_x10;
    uint32_t cpu_idle_x10;
} performance_monitor_status_t;

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
/** Copy the latest completed report; never measures, allocates, or logs. */
esp_err_t performance_monitor_get_status(performance_monitor_status_t *status);
