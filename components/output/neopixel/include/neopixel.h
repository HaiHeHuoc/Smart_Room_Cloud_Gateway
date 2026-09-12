#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEOPIXEL_COLOR_OFF      0x000000u
#define NEOPIXEL_COLOR_RED      0xFF0000u
#define NEOPIXEL_COLOR_GREEN    0x00FF00u
#define NEOPIXEL_COLOR_BLUE     0x0000FFu
#define NEOPIXEL_COLOR_WHITE    0xFFFFFFu
#define NEOPIXEL_COLOR_YELLOW   0xFFFF00u
#define NEOPIXEL_COLOR_CYAN     0x00FFFFu
#define NEOPIXEL_COLOR_MAGENTA  0xFF00FFu
#define NEOPIXEL_COLOR_ORANGE   0xFF8000u
#define NEOPIXEL_COLOR_PURPLE   0x8000FFu

/**
 * @brief Supported byte order for 3-channel addressable LEDs.
 *
 * v1.0 intentionally supports RGB and GRB only. RGBW/GRBW requires a public
 * white-channel API and is therefore outside the current component contract.
 */
typedef enum {
    NEOPIXEL_FORMAT_RGB = 0,
    NEOPIXEL_FORMAT_GRB,
} neopixel_format_t;

typedef enum {
    NEOPIXEL_EFFECT_NONE = 0,
    NEOPIXEL_EFFECT_SOLID,
    NEOPIXEL_EFFECT_BLINK,
    NEOPIXEL_EFFECT_FADE,
    NEOPIXEL_EFFECT_BREATH,
    NEOPIXEL_EFFECT_PULSE,
    NEOPIXEL_EFFECT_TRANSITION,
    NEOPIXEL_EFFECT_RAINBOW,
    NEOPIXEL_EFFECT_RAINBOW_CYCLE,
    NEOPIXEL_EFFECT_COLOR_WIPE,
    NEOPIXEL_EFFECT_CHASE,
    NEOPIXEL_EFFECT_GRADIENT,
    NEOPIXEL_EFFECT_THEATER_CHASE,
} neopixel_effect_t;

typedef struct {
    gpio_num_t gpio_num;
    size_t led_count;
    neopixel_format_t format;
    uint8_t default_brightness; /* 0..100 */
} neopixel_config_t;

/**
 * @brief Generic effect configuration.
 *
 * Fields are effect-specific. A zero duration/speed selects the library
 * default where documented. step_ms controls the requested update resolution;
 * values below the component task tick are effectively clamped to that tick.
 */
typedef struct {
    uint32_t color;
    uint32_t color_to;
    uint8_t brightness;
    uint8_t brightness_from;
    uint8_t brightness_to;
    uint32_t duration_ms;
    uint32_t speed_ms;
    uint32_t on_time_ms;
    uint32_t off_time_ms;
    uint32_t count;
    uint32_t step_ms;     /* 0 = component default update interval */
    uint16_t hue_offset;  /* degrees between LEDs for strip effects */
} neopixel_effect_config_t;

/**
 * @brief Initialize the singleton NeoPixel component.
 *
 * Runtime APIs are serialized internally and may be called from different
 * FreeRTOS tasks after successful initialization. init/deinit must be
 * externally serialized and no public API may race with deinit. APIs are not
 * ISR-safe.
 */
esp_err_t neopixel_init(const neopixel_config_t *config);

/**
 * @brief Stop the worker, turn the strip off, and release resources.
 *
 * Worker shutdown uses an internal synchronization handshake; this API does
 * not poll a shared TaskHandle_t. The lifecycle boundary itself must still be
 * externally serialized against other public API calls.
 */
esp_err_t neopixel_deinit(void);

/**
 * @brief Clear the logical buffer, stop all effects, and turn all LEDs off.
 */
esp_err_t neopixel_clear(void);

/**
 * @brief Re-render the current logical/output state to the physical LEDs.
 */
esp_err_t neopixel_show(void);

/* Basic color control.
 *
 * Whole-strip setters stop the strip effect and every per-pixel effect.
 * Per-pixel setters stop the strip effect and only the effect owned by the
 * addressed pixel; effects on other pixels continue running. */
esp_err_t neopixel_set_color(uint8_t r, uint8_t g, uint8_t b);
esp_err_t neopixel_set_color_hex(uint32_t hex_color);
/**
 * @brief Atomically apply static whole-strip logical state and render it once.
 *
 * This stops active effects, preserves the requested color and brightness even
 * when @p power_on is false, and clears the physical LEDs while off. The call
 * is intended for product-level state owners that must avoid intermediate
 * color or brightness frames between several independent setters.
 *
 * @param[in] hex_color RGB color in 0xRRGGBB form.
 * @param[in] brightness Output brightness in the inclusive range 0..100.
 * @param[in] power_on True to render the requested state; false to clear the
 *                     LEDs while retaining the requested logical state.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before initialization,
 *         ESP_ERR_INVALID_ARG for brightness above 100, ESP_ERR_TIMEOUT when
 *         the internal runtime lock cannot be obtained, or a led_strip error.
 */
esp_err_t neopixel_set_static_state(uint32_t hex_color,
                                    uint8_t brightness,
                                    bool power_on);
esp_err_t neopixel_set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b);
esp_err_t neopixel_set_pixel_hex(size_t index, uint32_t hex_color);
esp_err_t neopixel_clear_pixel(size_t index);
esp_err_t neopixel_fill(uint8_t r, uint8_t g, uint8_t b);
esp_err_t neopixel_fill_hex(uint32_t hex_color);

/* Brightness is stored independently from logical RGB values. */
esp_err_t neopixel_set_brightness(uint8_t percent);
uint8_t neopixel_get_brightness(void);

/* OFF preserves logical state and pauses all running effects. ON resumes only
 * effects that were paused by OFF. Explicitly paused effects remain paused. */
esp_err_t neopixel_on(void);
esp_err_t neopixel_off(void);
esp_err_t neopixel_toggle(void);
bool neopixel_is_on(void);

/* For multi-LED patterns, get_color() reports the current/primary strip color,
 * not a complete strip snapshot. */
uint32_t neopixel_get_color(void);
neopixel_effect_t neopixel_get_effect(void);

/* Convenience non-blocking whole-strip effects. Starting a whole-strip effect
 * stops all existing per-pixel effects. */
esp_err_t neopixel_blink(uint32_t color, uint8_t brightness,
                         uint32_t on_time_ms, uint32_t off_time_ms);
esp_err_t neopixel_fade_in(uint32_t color, uint32_t duration_ms);
esp_err_t neopixel_fade_out(uint32_t duration_ms);
esp_err_t neopixel_fade(uint32_t color, uint8_t brightness_from,
                        uint8_t brightness_to, uint32_t duration_ms);
esp_err_t neopixel_transition(uint32_t from_color, uint32_t to_color,
                              uint32_t duration_ms);
esp_err_t neopixel_rainbow(uint32_t duration_ms);
esp_err_t neopixel_rainbow_cycle(uint32_t speed_ms);
esp_err_t neopixel_breath(uint32_t color, uint32_t period_ms);
esp_err_t neopixel_pulse(uint32_t color, uint8_t brightness,
                         uint32_t duration_ms, uint32_t count);
esp_err_t neopixel_chase(uint32_t color, uint32_t speed_ms);
esp_err_t neopixel_color_wipe(uint32_t color, uint32_t speed_ms);
esp_err_t neopixel_theater_chase(uint32_t color, uint32_t speed_ms);
esp_err_t neopixel_gradient(uint32_t color_start, uint32_t color_end);
esp_err_t neopixel_rainbow_gradient(void);

/* Generic whole-strip effect control.
 *
 * stop_effect() freezes the current logical/output frame. It does not restore
 * the state that existed before the effect was started. */
esp_err_t neopixel_start_effect(neopixel_effect_t effect,
                                const neopixel_effect_config_t *config);
esp_err_t neopixel_stop_effect(void);
esp_err_t neopixel_pause_effect(void);
esp_err_t neopixel_resume_effect(void);

/**
 * @brief Start or replace an effect on one LED only.
 *
 * Starting a per-pixel effect stops any active whole-strip effect, but does not
 * disturb effects owned by other pixels. The following effects are supported
 * per pixel: SOLID, BLINK, FADE, BREATH, PULSE, TRANSITION, RAINBOW, and
 * RAINBOW_CYCLE. Strip-spatial effects (WIPE/CHASE/GRADIENT/THEATER_CHASE)
 * return ESP_ERR_NOT_SUPPORTED.
 *
 * stop_pixel_effect() freezes the target LED at its current logical/output
 * frame. stop_all_pixel_effects() applies the same freeze semantics to every
 * independently animated LED.
 */
esp_err_t neopixel_set_pixel_effect(size_t index,
                                     neopixel_effect_t effect,
                                     const neopixel_effect_config_t *config);
esp_err_t neopixel_stop_pixel_effect(size_t index);
esp_err_t neopixel_pause_pixel_effect(size_t index);
esp_err_t neopixel_resume_pixel_effect(size_t index);
esp_err_t neopixel_stop_all_pixel_effects(void);
esp_err_t neopixel_get_pixel_effect(size_t index,
                                    neopixel_effect_t *out_effect);
esp_err_t neopixel_get_pixel_color(size_t index, uint32_t *out_color);

/* Pure color utilities; these do not require neopixel_init(). */
uint32_t neopixel_rgb_to_hex(uint8_t r, uint8_t g, uint8_t b);
void neopixel_hex_to_rgb(uint32_t hex_color, uint8_t *r, uint8_t *g,
                         uint8_t *b);
uint32_t neopixel_hsv_to_rgb(float h, float s, float v);
void neopixel_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, float *h,
                         float *s, float *v);
uint32_t neopixel_color_lerp(uint32_t color_a, uint32_t color_b, float ratio);

#ifdef __cplusplus
}
#endif
