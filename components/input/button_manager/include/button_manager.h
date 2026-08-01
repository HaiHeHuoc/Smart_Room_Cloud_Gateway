#pragma once

/* Includes ----------------------------------------------------------------- */
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Type Definitions -------------------------------------------------------- */

/**
 * @brief Stable button events published by the polling task.
 */
typedef enum
{
    /** A physical press remained active for the configured debounce time. */
    BUTTON_MANAGER_EVENT_PRESSED = 0,

    /** A physical release remained inactive for the debounce time. */
    BUTTON_MANAGER_EVENT_RELEASED,

    /** The stable press reached the configured long-press duration. */
    BUTTON_MANAGER_EVENT_LONG_PRESS
} button_manager_event_t;

/**
 * @brief Immutable event snapshot supplied to the application callback.
 */
typedef struct
{
    /** Event detected by the button state machine. */
    button_manager_event_t event;

    /**
     * Stable hold duration in milliseconds.
     *
     * This is zero for BUTTON_MANAGER_EVENT_PRESSED.
     */
    uint32_t held_ms;
} button_manager_event_data_t;

/**
 * @brief Application callback invoked from the button polling task.
 *
 * The callback must return quickly. It must not perform blocking storage,
 * networking, BLE provisioning, LVGL operations, or reboot the device.
 *
 * The event pointer is valid only until the callback returns.
 */
typedef void (*button_manager_event_callback_t)(
    const button_manager_event_data_t *event_data,
    void *user_context);

/**
 * @brief Runtime configuration copied during initialization.
 */
typedef struct
{
    /** GPIO connected to the physical button. */
    gpio_num_t gpio_num;

    /**
     * GPIO level representing the pressed state.
     *
     * Use zero for an active-low button connected to GND.
     */
    uint8_t active_level;

    /** GPIO polling period in milliseconds. */
    uint32_t poll_period_ms;

    /** Required stable time before accepting an edge. */
    uint32_t debounce_ms;

    /** Stable hold duration required for a long-press event. */
    uint32_t long_press_ms;
} button_manager_config_t;

/* Functions ---------------------------------------------------------------- */

/**
 * @brief Configure the button GPIO and initialize manager state.
 *
 * This function copies @p config, configures the GPIO as an input, and selects
 * an internal pull-up for active-low input or pull-down for active-high input.
 * It does not create the polling task and must be called once from normal task
 * context before callback registration.
 *
 * @param[in] config GPIO and timing configuration. `poll_period_ms` must map to
 *                   at least one OS tick, `debounce_ms` must be greater than
 *                   `poll_period_ms`, and `long_press_ms` must be greater than
 *                   `poll_period_ms`.
 *
 * @return
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG for an invalid configuration.
 * - ESP_ERR_INVALID_STATE if already initialized.
 * - GPIO driver error returned by gpio_config().
 */
esp_err_t button_manager_init(
    const button_manager_config_t *config);

/**
 * @brief Register the callback used for button events.
 *
 * Registration is allowed only after button_manager_init() and before
 * button_manager_start(). The manager retains @p callback and @p user_context
 * for the lifetime of the polling task; the caller must keep any referenced
 * context valid. The callback runs in button-task context with no manager lock
 * held and must return quickly.
 *
 * @param[in] callback Non-NULL event callback.
 * @param[in] user_context Opaque context returned to the callback; may be NULL.
 *
 * @return
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG if callback is NULL.
 * - ESP_ERR_INVALID_STATE outside the registration window.
 */
esp_err_t button_manager_register_callback(
    button_manager_event_callback_t callback,
    void *user_context);

/**
 * @brief Create and start the button polling task.
 *
 * A callback must have been registered first. This API is non-blocking after
 * task creation. The task uses a 3072-byte stack, priority 4, polls with the
 * configured finite period, and has no stop/deinit operation in Phase 7.1.
 *
 * @return
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_STATE if lifecycle requirements are not met.
 * - ESP_ERR_NO_MEM if the task cannot be created.
 */
esp_err_t button_manager_start(void);

#ifdef __cplusplus
}
#endif
