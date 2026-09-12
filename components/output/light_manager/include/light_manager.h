#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Byte order of a three-channel WS2812-compatible NeoPixel. */
typedef enum
{
    LIGHT_MANAGER_PIXEL_FORMAT_RGB = 0,
    LIGHT_MANAGER_PIXEL_FORMAT_GRB,
} light_manager_pixel_format_t;

/**
 * @brief Board-supplied initialization configuration.
 *
 * The component never selects a GPIO itself. The composition root supplies
 * the physical pin, LED count, byte order, and boot-time logical brightness.
 */
typedef struct
{
    gpio_num_t gpio_num;
    size_t led_count;
    light_manager_pixel_format_t pixel_format;
    uint8_t default_brightness_percent;
} light_manager_config_t;

/**
 * @brief Copyable product-level state for static light control.
 *
 * RGB remains logical color data when @c power_on is false. Brightness zero is
 * valid and means a zero-output ON state; it does not alter the stored RGB.
 */
typedef struct
{
    bool power_on;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness_percent;
} light_manager_state_t;

/**
 * @brief Initialize the singleton light manager and leave the LEDs OFF.
 *
 * @param[in] config Board-specific immutable configuration.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid configuration,
 *         ESP_ERR_INVALID_STATE when already initialized, or an underlying
 *         NeoPixel/ESP-IDF error.
 *
 * @note Safe from normal FreeRTOS task context only. The API serializes calls
 *       internally; it is not ISR-safe and must not be called from an ISR.
 */
esp_err_t light_manager_init(const light_manager_config_t *config);

/**
 * @brief Turn LEDs OFF, release the NeoPixel driver, and clear manager state.
 *
 * @return ESP_OK on success or ESP_ERR_INVALID_STATE before initialization.
 *
 * @note Call only from normal task context. Concurrent public API callers are
 *       serialized. No caller may retain assumptions about state after this
 *       function returns.
 */
esp_err_t light_manager_deinit(void);

/**
 * @brief Atomically apply power, RGB, and brightness as one product request.
 *
 * @param[in] state Requested logical state. Brightness must be 0..100.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for NULL or invalid
 *         brightness, ESP_ERR_INVALID_STATE before initialization, or an
 *         underlying NeoPixel/ESP-IDF error.
 */
esp_err_t light_manager_set_state(const light_manager_state_t *state);

/** @brief Replace logical RGB while preserving current power and brightness. */
esp_err_t light_manager_set_color(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Replace logical brightness while preserving current power and RGB.
 *
 * @return ESP_ERR_INVALID_ARG when @p brightness_percent exceeds 100.
 */
esp_err_t light_manager_set_brightness(uint8_t brightness_percent);

/** @brief Set power ON while preserving the stored RGB and brightness. */
esp_err_t light_manager_on(void);

/** @brief Set power OFF while preserving the stored RGB and brightness. */
esp_err_t light_manager_off(void);

/**
 * @brief Copy the current logical product state.
 *
 * This never exposes NeoPixel, RMT, LED-strip, or mutex implementation data.
 */
esp_err_t light_manager_get_state(light_manager_state_t *state);

#ifdef __cplusplus
}
#endif
