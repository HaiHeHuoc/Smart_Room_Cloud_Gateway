#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t gpio_num;
    uint8_t active_level;
    uint32_t poll_period_ms;
    uint32_t debounce_ms;
} voice_assistant_ptt_gpio_config_t;

/**
 * @brief Configure one dedicated active-high/active-low PTT GPIO.
 *
 * The adapter owns only GPIO sampling/debounce. It does not own the factory
 * reset button and forwards stable edges to voice_assistant_ptt_press/release.
 * A stable edge that is temporarily rejected by the bounded policy queue is
 * retried while the sampled level remains unchanged, so queue pressure cannot
 * silently discard one physical press or release.
 * GPIO uses any-edge interrupts to wake the debounce task and has the ESP32-S3
 * internal pull-down enabled. Phase 14's active-high GPIO38 switch also
 * requires a 10 kOhm external pull-down at the physical switch; software
 * cannot identify a release while the electrical level remains HIGH.
 */
esp_err_t voice_assistant_ptt_gpio_init(
    const voice_assistant_ptt_gpio_config_t *config);

/** Start the bounded polling/debounce task. */
esp_err_t voice_assistant_ptt_gpio_start(void);

#ifdef __cplusplus
}
#endif
