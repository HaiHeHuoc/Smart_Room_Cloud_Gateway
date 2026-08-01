#pragma once

/* Includes ----------------------------------------------------------------- */
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief Qualified physical-button inputs accepted by the reset coordinator.
 *
 * Values are copied into the coordinator queue; the caller retains no data.
 */
typedef enum
{
    /** Begin one physical press cycle. */
    APP_RESET_COORDINATOR_INPUT_PRESSED = 0,

    /** Request one reset action for the active press cycle. */
    APP_RESET_COORDINATOR_INPUT_LONG_PRESS,

    /** End the current press cycle and re-arm qualification. */
    APP_RESET_COORDINATOR_INPUT_RELEASED
} app_reset_coordinator_input_event_t;

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Allocate the reset coordinator input queue.
 *
 * Call once from application startup before app_reset_coordinator_start().
 * This function does not create a task, access storage, or perform a reset.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE after prior initialization,
 *         or ESP_ERR_NO_MEM when queue allocation fails.
 */
esp_err_t app_reset_coordinator_init(void);

/**
 * @brief Start the permanent reset qualification task.
 *
 * Call once after app_reset_coordinator_init(). The function returns after
 * task creation and does not wait for an input event. Phase 7.2 provides no
 * stop or deinitialization operation.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE when not initialized, or
 *         ESP_ERR_NO_MEM when task creation fails.
 */
esp_err_t app_reset_coordinator_start(void);

/**
 * @brief Post one copied button input without blocking the caller.
 *
 * This API is task-context only and is safe for the button task callback. It
 * performs no storage, Wi-Fi, provisioning, reboot, or LVGL operation.
 *
 * @param[in] event Input event to copy into the coordinator queue.
 * @return ESP_OK when queued, ESP_ERR_INVALID_ARG for an unknown event,
 *         ESP_ERR_INVALID_STATE before the task is running, or ESP_ERR_TIMEOUT
 *         when the queue is full.
 */
esp_err_t app_reset_coordinator_post_input_event(
    app_reset_coordinator_input_event_t event);

#ifdef __cplusplus
}
#endif
