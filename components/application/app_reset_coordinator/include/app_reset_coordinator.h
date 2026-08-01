#pragma once

/**
 * @file app_reset_coordinator.h
 * @brief Public task-context API for qualified physical Wi-Fi factory reset.
 *
 * The component accepts copied button events, executes reset work only from
 * its dedicated task, coordinates network quiescence before persistent
 * erasure, and publishes reset-result state through the GUI owner. It never
 * performs reset work in the button callback and exposes no ISR-safe API.
 *
 * Phase 7.6 closes documentation and hardware acceptance only; the public API
 * and runtime behavior declared here are unchanged.
 */

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
 * Values are copied into the coordinator queue. The caller retains ownership
 * of its source data and may release or reuse it immediately after the call.
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
 * Call once from normal application task context before
 * app_reset_coordinator_start(). This function does not create a task, access
 * storage, touch Wi-Fi/provisioning state, publish GUI work, or reset the
 * device.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE after prior initialization,
 *         or ESP_ERR_NO_MEM when queue allocation fails.
 */
esp_err_t app_reset_coordinator_init(void);

/**
 * @brief Start the permanent reset qualification and execution task.
 *
 * Call once after app_reset_coordinator_init(). The function returns after
 * task creation and does not wait for an input event. The component provides
 * no stop or deinitialization operation. The created task owns persistent
 * reset execution, network-quiescence waiting, copied reset-result
 * publication, bounded presentation waiting, and the verified-success reboot;
 * callers never wait for those operations.
 *
 * This function is task-context only and is not ISR-safe.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE when not initialized, or
 *         ESP_ERR_NO_MEM when task creation fails.
 */
esp_err_t app_reset_coordinator_start(void);

/**
 * @brief Post one copied button input without blocking the caller.
 *
 * This API is task-context only and is suitable for the button task callback.
 * It performs a zero-wait queue send and returns immediately. It performs no
 * storage, Wi-Fi, provisioning, reboot, allocation, delay, or LVGL operation.
 * Queue ownership transfers only for the copied enum value; no caller-owned
 * pointer is retained.
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
