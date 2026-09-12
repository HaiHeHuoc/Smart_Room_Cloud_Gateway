#include "neopixel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#define EFFECT_TICK_MS 20u
#define EFFECT_TASK_STACK 4096u
#define EFFECT_TASK_PRIO 5u
#define NEOPIXEL_LOCK_TIMEOUT_MS 1000u
#define NEOPIXEL_WORKER_STOP_TIMEOUT_MS 2000u
#define NEOPIXEL_PI_F 3.14159265358979323846f

static const char *TAG = "neopixel";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef struct {
    rgb_t color;
    uint8_t output_brightness;
    neopixel_effect_t effect;
    neopixel_effect_config_t effect_cfg;
    bool effect_paused;
    bool effect_paused_by_off;
    bool first_effect_update;
    uint64_t effect_elapsed_ms;
    uint64_t last_effect_update_ms;
} neopixel_pixel_state_t;

typedef struct {
    bool initialized;
    bool on;
    bool effect_paused;
    bool effect_paused_by_off;
    bool first_effect_update;
    bool task_running;
    gpio_num_t gpio;
    size_t count;
    neopixel_format_t format;
    uint8_t brightness;
    uint32_t current_color;
    neopixel_pixel_state_t *pixels;
    led_strip_handle_t strip;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t worker_stopped;
    TaskHandle_t effect_task;
    neopixel_effect_t effect;
    neopixel_effect_config_t effect_cfg;
    uint64_t effect_elapsed_ms;
    uint64_t last_effect_update_ms;
} neopixel_ctx_t;

static neopixel_ctx_t s;

static inline uint8_t scale8(uint8_t value, uint8_t percent)
{
    return (uint8_t)(((uint16_t)value * percent + 50u) / 100u);
}

static bool lock_ctx(void)
{
    return s.lock &&
           xSemaphoreTake(s.lock,
                          pdMS_TO_TICKS(NEOPIXEL_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void unlock_ctx(void)
{
    xSemaphoreGive(s.lock);
}

static bool runtime_ready(void)
{
    return s.initialized && s.lock && s.worker_stopped && s.strip && s.pixels;
}

uint32_t neopixel_rgb_to_hex(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void neopixel_hex_to_rgb(uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r) {
        *r = (uint8_t)(color >> 16);
    }
    if (g) {
        *g = (uint8_t)(color >> 8);
    }
    if (b) {
        *b = (uint8_t)color;
    }
}

uint32_t neopixel_color_lerp(uint32_t a, uint32_t b, float ratio)
{
    if (ratio < 0.0f) {
        ratio = 0.0f;
    }
    if (ratio > 1.0f) {
        ratio = 1.0f;
    }

    uint8_t ar, ag, ab, br, bg, bb;
    neopixel_hex_to_rgb(a, &ar, &ag, &ab);
    neopixel_hex_to_rgb(b, &br, &bg, &bb);

    return neopixel_rgb_to_hex(
        (uint8_t)(ar + (br - ar) * ratio),
        (uint8_t)(ag + (bg - ag) * ratio),
        (uint8_t)(ab + (bb - ab) * ratio));
}

uint32_t neopixel_hsv_to_rgb(float h, float sat, float val)
{
    while (h < 0.0f) {
        h += 360.0f;
    }
    while (h >= 360.0f) {
        h -= 360.0f;
    }

    if (sat < 0.0f) {
        sat = 0.0f;
    }
    if (sat > 1.0f) {
        sat = 1.0f;
    }
    if (val < 0.0f) {
        val = 0.0f;
    }
    if (val > 1.0f) {
        val = 1.0f;
    }

    float c = val * sat;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = val - c;
    float rf = 0.0f;
    float gf = 0.0f;
    float bf = 0.0f;

    if (h < 60.0f) {
        rf = c;
        gf = x;
    } else if (h < 120.0f) {
        rf = x;
        gf = c;
    } else if (h < 180.0f) {
        gf = c;
        bf = x;
    } else if (h < 240.0f) {
        gf = x;
        bf = c;
    } else if (h < 300.0f) {
        rf = x;
        bf = c;
    } else {
        rf = c;
        bf = x;
    }

    return neopixel_rgb_to_hex(
        (uint8_t)((rf + m) * 255.0f),
        (uint8_t)((gf + m) * 255.0f),
        (uint8_t)((bf + m) * 255.0f));
}

void neopixel_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b,
                         float *h, float *sat, float *val)
{
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;
    float maxv = fmaxf(rf, fmaxf(gf, bf));
    float minv = fminf(rf, fminf(gf, bf));
    float delta = maxv - minv;
    float hue = 0.0f;

    if (delta != 0.0f) {
        if (maxv == rf) {
            hue = 60.0f * fmodf((gf - bf) / delta, 6.0f);
        } else if (maxv == gf) {
            hue = 60.0f * (((bf - rf) / delta) + 2.0f);
        } else {
            hue = 60.0f * (((rf - gf) / delta) + 4.0f);
        }

        if (hue < 0.0f) {
            hue += 360.0f;
        }
    }

    if (h) {
        *h = hue;
    }
    if (sat) {
        *sat = maxv == 0.0f ? 0.0f : delta / maxv;
    }
    if (val) {
        *val = maxv;
    }
}

static void set_pixel_color_locked(size_t index, uint32_t color)
{
    neopixel_hex_to_rgb(color,
                        &s.pixels[index].color.r,
                        &s.pixels[index].color.g,
                        &s.pixels[index].color.b);
}

static uint32_t get_pixel_color_locked(size_t index)
{
    return neopixel_rgb_to_hex(s.pixels[index].color.r,
                               s.pixels[index].color.g,
                               s.pixels[index].color.b);
}

static void set_all_output_brightness_locked(uint8_t brightness)
{
    for (size_t i = 0; i < s.count; ++i) {
        s.pixels[i].output_brightness = brightness;
    }
}

static void fill_logical_locked(uint32_t color)
{
    for (size_t i = 0; i < s.count; ++i) {
        set_pixel_color_locked(i, color);
    }
    s.current_color = color & 0xFFFFFFu;
}

static esp_err_t render_locked(void)
{
    if (!s.on) {
        return led_strip_clear(s.strip);
    }

    for (size_t i = 0; i < s.count; ++i) {
        const neopixel_pixel_state_t *pixel = &s.pixels[i];
        esp_err_t err = led_strip_set_pixel(
            s.strip,
            i,
            scale8(pixel->color.r, pixel->output_brightness),
            scale8(pixel->color.g, pixel->output_brightness),
            scale8(pixel->color.b, pixel->output_brightness));
        if (err != ESP_OK) {
            return err;
        }
    }

    return led_strip_refresh(s.strip);
}

static void stop_strip_effect_locked(void)
{
    s.effect = NEOPIXEL_EFFECT_NONE;
    s.effect_paused = false;
    s.effect_paused_by_off = false;
    s.first_effect_update = false;
    s.effect_elapsed_ms = 0;
    s.last_effect_update_ms = 0;
    memset(&s.effect_cfg, 0, sizeof(s.effect_cfg));
}

static void stop_pixel_effect_state_locked(neopixel_pixel_state_t *pixel)
{
    pixel->effect = NEOPIXEL_EFFECT_NONE;
    pixel->effect_paused = false;
    pixel->effect_paused_by_off = false;
    pixel->first_effect_update = false;
    pixel->effect_elapsed_ms = 0;
    pixel->last_effect_update_ms = 0;
    memset(&pixel->effect_cfg, 0, sizeof(pixel->effect_cfg));
}

static void stop_all_pixel_effects_locked(void)
{
    for (size_t i = 0; i < s.count; ++i) {
        stop_pixel_effect_state_locked(&s.pixels[i]);
    }
}

static bool has_pixel_effects_locked(void)
{
    for (size_t i = 0; i < s.count; ++i) {
        if (s.pixels[i].effect != NEOPIXEL_EFFECT_NONE) {
            return true;
        }
    }
    return false;
}

static uint32_t effective_step_ms(const neopixel_effect_config_t *cfg)
{
    if (cfg->step_ms == 0 || cfg->step_ms < EFFECT_TICK_MS) {
        return EFFECT_TICK_MS;
    }
    return cfg->step_ms;
}

static bool effect_update_due(uint64_t elapsed_ms,
                              uint64_t *last_update_ms,
                              bool *first_update,
                              const neopixel_effect_config_t *cfg)
{
    if (*first_update) {
        *first_update = false;
        *last_update_ms = elapsed_ms;
        return true;
    }

    uint64_t step = effective_step_ms(cfg);
    if ((elapsed_ms - *last_update_ms) < step) {
        return false;
    }

    *last_update_ms = elapsed_ms;
    return true;
}

static esp_err_t validate_effect_config(neopixel_effect_t effect,
                                        const neopixel_effect_config_t *cfg,
                                        bool pixel_scope)
{
    if (!cfg ||
        effect <= NEOPIXEL_EFFECT_NONE ||
        effect > NEOPIXEL_EFFECT_THEATER_CHASE ||
        cfg->brightness > 100 ||
        cfg->brightness_from > 100 ||
        cfg->brightness_to > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    if (effect == NEOPIXEL_EFFECT_BLINK &&
        cfg->on_time_ms == 0 && cfg->off_time_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (pixel_scope &&
        (effect == NEOPIXEL_EFFECT_COLOR_WIPE ||
         effect == NEOPIXEL_EFFECT_CHASE ||
         effect == NEOPIXEL_EFFECT_GRADIENT ||
         effect == NEOPIXEL_EFFECT_THEATER_CHASE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}

static bool update_strip_effect_locked(uint32_t dt_ms)
{
    if (s.effect == NEOPIXEL_EFFECT_NONE || s.effect_paused || !s.on) {
        return false;
    }

    s.effect_elapsed_ms += dt_ms;
    if (!effect_update_due(s.effect_elapsed_ms,
                           &s.last_effect_update_ms,
                           &s.first_effect_update,
                           &s.effect_cfg)) {
        return false;
    }

    neopixel_effect_config_t *cfg = &s.effect_cfg;
    uint64_t elapsed = s.effect_elapsed_ms;

    switch (s.effect) {
    case NEOPIXEL_EFFECT_SOLID:
        fill_logical_locked(cfg->color);
        s.brightness = cfg->brightness;
        set_all_output_brightness_locked(s.brightness);
        stop_strip_effect_locked();
        return true;

    case NEOPIXEL_EFFECT_BLINK: {
        uint64_t on_ms = cfg->on_time_ms ? cfg->on_time_ms : 500u;
        uint64_t off_ms = cfg->off_time_ms ? cfg->off_time_ms : 500u;
        uint64_t cycle = on_ms + off_ms;
        bool lit = (elapsed % cycle) < on_ms;

        fill_logical_locked(cfg->color);
        set_all_output_brightness_locked(lit ? cfg->brightness : 0);
        return true;
    }

    case NEOPIXEL_EFFECT_FADE: {
        uint64_t duration = cfg->duration_ms ? cfg->duration_ms : 1u;
        float ratio = elapsed >= duration ? 1.0f
                                          : (float)elapsed / (float)duration;
        int delta = (int)cfg->brightness_to - (int)cfg->brightness_from;
        uint8_t brightness = (uint8_t)(
            (float)cfg->brightness_from + delta * ratio);

        fill_logical_locked(cfg->color);
        set_all_output_brightness_locked(brightness);

        if (elapsed >= duration) {
            s.brightness = cfg->brightness_to;
            stop_strip_effect_locked();
        }
        return true;
    }

    case NEOPIXEL_EFFECT_TRANSITION: {
        uint64_t duration = cfg->duration_ms ? cfg->duration_ms : 1u;
        float ratio = elapsed >= duration ? 1.0f
                                          : (float)elapsed / (float)duration;
        uint32_t color = neopixel_color_lerp(cfg->color,
                                             cfg->color_to,
                                             ratio);

        fill_logical_locked(color);
        set_all_output_brightness_locked(s.brightness);

        if (elapsed >= duration) {
            fill_logical_locked(cfg->color_to);
            stop_strip_effect_locked();
        }
        return true;
    }

    case NEOPIXEL_EFFECT_BREATH: {
        uint64_t period = cfg->duration_ms ? cfg->duration_ms : 2000u;
        float phase = (float)(elapsed % period) / (float)period;
        float wave = 0.5f - 0.5f * cosf(phase * 2.0f * NEOPIXEL_PI_F);

        fill_logical_locked(cfg->color);
        set_all_output_brightness_locked((uint8_t)(s.brightness * wave));
        return true;
    }

    case NEOPIXEL_EFFECT_PULSE: {
        uint64_t duration = cfg->duration_ms ? cfg->duration_ms : 300u;
        uint64_t pulse_count = cfg->count ? cfg->count : 1u;
        uint64_t position = elapsed % duration;
        float ratio = (float)position / (float)duration;
        float wave = ratio < 0.5f
                         ? ratio * 2.0f
                         : (1.0f - ratio) * 2.0f;

        fill_logical_locked(cfg->color);
        set_all_output_brightness_locked(
            (uint8_t)(cfg->brightness * wave));

        if (elapsed >= duration * pulse_count) {
            s.current_color = cfg->color & 0xFFFFFFu;
            set_all_output_brightness_locked(0);
            stop_strip_effect_locked();
        }
        return true;
    }

    case NEOPIXEL_EFFECT_RAINBOW:
    case NEOPIXEL_EFFECT_RAINBOW_CYCLE: {
        uint64_t period;
        if (cfg->duration_ms) {
            period = cfg->duration_ms;
        } else if (cfg->speed_ms) {
            period = (uint64_t)cfg->speed_ms * 360u;
        } else {
            period = 3600u;
        }

        float base = 360.0f * (float)(elapsed % period) / (float)period;
        for (size_t i = 0; i < s.count; ++i) {
            uint32_t color = neopixel_hsv_to_rgb(
                base + (float)(i * cfg->hue_offset),
                1.0f,
                1.0f);
            set_pixel_color_locked(i, color);
            s.pixels[i].output_brightness = s.brightness;
        }

        s.current_color = neopixel_hsv_to_rgb(base, 1.0f, 1.0f);
        if (s.effect == NEOPIXEL_EFFECT_RAINBOW &&
            cfg->duration_ms &&
            elapsed >= cfg->duration_ms) {
            stop_strip_effect_locked();
        }
        return true;
    }

    case NEOPIXEL_EFFECT_COLOR_WIPE:
    case NEOPIXEL_EFFECT_CHASE:
    case NEOPIXEL_EFFECT_THEATER_CHASE: {
        uint64_t speed = cfg->speed_ms ? cfg->speed_ms : 100u;
        size_t position = (size_t)((elapsed / speed) % s.count);

        for (size_t i = 0; i < s.count; ++i) {
            set_pixel_color_locked(i, NEOPIXEL_COLOR_OFF);
            s.pixels[i].output_brightness = s.brightness;
        }

        if (s.effect == NEOPIXEL_EFFECT_COLOR_WIPE) {
            for (size_t i = 0; i <= position; ++i) {
                set_pixel_color_locked(i, cfg->color);
            }
        } else if (s.effect == NEOPIXEL_EFFECT_THEATER_CHASE) {
            for (size_t i = position % 3; i < s.count; i += 3) {
                set_pixel_color_locked(i, cfg->color);
            }
        } else {
            set_pixel_color_locked(position, cfg->color);
        }

        s.current_color = cfg->color & 0xFFFFFFu;
        return true;
    }

    case NEOPIXEL_EFFECT_GRADIENT:
        for (size_t i = 0; i < s.count; ++i) {
            float ratio = s.count <= 1
                              ? 0.0f
                              : (float)i / (float)(s.count - 1);
            uint32_t color = neopixel_color_lerp(cfg->color,
                                                 cfg->color_to,
                                                 ratio);
            set_pixel_color_locked(i, color);
            s.pixels[i].output_brightness = s.brightness;
        }
        s.current_color = cfg->color_to & 0xFFFFFFu;
        stop_strip_effect_locked();
        return true;

    case NEOPIXEL_EFFECT_NONE:
    default:
        stop_strip_effect_locked();
        return false;
    }
}

static bool update_pixel_effect_locked(size_t index, uint32_t dt_ms)
{
    neopixel_pixel_state_t *pixel = &s.pixels[index];

    if (pixel->effect == NEOPIXEL_EFFECT_NONE ||
        pixel->effect_paused ||
        !s.on) {
        return false;
    }

    pixel->effect_elapsed_ms += dt_ms;
    if (!effect_update_due(pixel->effect_elapsed_ms,
                           &pixel->last_effect_update_ms,
                           &pixel->first_effect_update,
                           &pixel->effect_cfg)) {
        return false;
    }

    neopixel_effect_config_t *cfg = &pixel->effect_cfg;
    uint64_t elapsed = pixel->effect_elapsed_ms;

    switch (pixel->effect) {
    case NEOPIXEL_EFFECT_SOLID:
        set_pixel_color_locked(index, cfg->color);
        pixel->output_brightness = cfg->brightness;
        stop_pixel_effect_state_locked(pixel);
        return true;

    case NEOPIXEL_EFFECT_BLINK: {
        uint64_t on_ms = cfg->on_time_ms ? cfg->on_time_ms : 500u;
        uint64_t off_ms = cfg->off_time_ms ? cfg->off_time_ms : 500u;
        uint64_t cycle = on_ms + off_ms;
        bool lit = (elapsed % cycle) < on_ms;

        set_pixel_color_locked(index, cfg->color);
        pixel->output_brightness = lit ? cfg->brightness : 0;
        return true;
    }

    case NEOPIXEL_EFFECT_FADE: {
        uint64_t duration = cfg->duration_ms ? cfg->duration_ms : 1u;
        float ratio = elapsed >= duration ? 1.0f
                                          : (float)elapsed / (float)duration;
        int delta = (int)cfg->brightness_to - (int)cfg->brightness_from;

        set_pixel_color_locked(index, cfg->color);
        pixel->output_brightness = (uint8_t)(
            (float)cfg->brightness_from + delta * ratio);

        if (elapsed >= duration) {
            stop_pixel_effect_state_locked(pixel);
        }
        return true;
    }

    case NEOPIXEL_EFFECT_TRANSITION: {
        uint64_t duration = cfg->duration_ms ? cfg->duration_ms : 1u;
        float ratio = elapsed >= duration ? 1.0f
                                          : (float)elapsed / (float)duration;
        uint32_t color = neopixel_color_lerp(cfg->color,
                                             cfg->color_to,
                                             ratio);

        set_pixel_color_locked(index, color);
        pixel->output_brightness = s.brightness;

        if (elapsed >= duration) {
            set_pixel_color_locked(index, cfg->color_to);
            stop_pixel_effect_state_locked(pixel);
        }
        return true;
    }

    case NEOPIXEL_EFFECT_BREATH: {
        uint64_t period = cfg->duration_ms ? cfg->duration_ms : 2000u;
        float phase = (float)(elapsed % period) / (float)period;
        float wave = 0.5f - 0.5f * cosf(phase * 2.0f * NEOPIXEL_PI_F);

        set_pixel_color_locked(index, cfg->color);
        pixel->output_brightness = (uint8_t)(s.brightness * wave);
        return true;
    }

    case NEOPIXEL_EFFECT_PULSE: {
        uint64_t duration = cfg->duration_ms ? cfg->duration_ms : 300u;
        uint64_t pulse_count = cfg->count ? cfg->count : 1u;
        uint64_t position = elapsed % duration;
        float ratio = (float)position / (float)duration;
        float wave = ratio < 0.5f
                         ? ratio * 2.0f
                         : (1.0f - ratio) * 2.0f;

        set_pixel_color_locked(index, cfg->color);
        pixel->output_brightness = (uint8_t)(cfg->brightness * wave);

        if (elapsed >= duration * pulse_count) {
            pixel->output_brightness = 0;
            stop_pixel_effect_state_locked(pixel);
        }
        return true;
    }

    case NEOPIXEL_EFFECT_RAINBOW:
    case NEOPIXEL_EFFECT_RAINBOW_CYCLE: {
        uint64_t period;
        if (cfg->duration_ms) {
            period = cfg->duration_ms;
        } else if (cfg->speed_ms) {
            period = (uint64_t)cfg->speed_ms * 360u;
        } else {
            period = 3600u;
        }

        float hue = 360.0f * (float)(elapsed % period) / (float)period;
        set_pixel_color_locked(index,
                               neopixel_hsv_to_rgb(hue, 1.0f, 1.0f));
        pixel->output_brightness = s.brightness;

        if (pixel->effect == NEOPIXEL_EFFECT_RAINBOW &&
            cfg->duration_ms &&
            elapsed >= cfg->duration_ms) {
            stop_pixel_effect_state_locked(pixel);
        }
        return true;
    }

    case NEOPIXEL_EFFECT_COLOR_WIPE:
    case NEOPIXEL_EFFECT_CHASE:
    case NEOPIXEL_EFFECT_GRADIENT:
    case NEOPIXEL_EFFECT_THEATER_CHASE:
    case NEOPIXEL_EFFECT_NONE:
    default:
        stop_pixel_effect_state_locked(pixel);
        return false;
    }
}

static void stop_all_effects_on_render_error_locked(esp_err_t err)
{
    ESP_LOGE(TAG, "NeoPixel render failed: %s", esp_err_to_name(err));
    stop_strip_effect_locked();
    stop_all_pixel_effects_locked();
}

static void effect_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(EFFECT_TICK_MS));

        if (!lock_ctx()) {
            continue;
        }

        if (!s.task_running) {
            unlock_ctx();
            break;
        }

        bool dirty = false;
        if (s.effect != NEOPIXEL_EFFECT_NONE) {
            dirty = update_strip_effect_locked(EFFECT_TICK_MS);
        } else if (has_pixel_effects_locked()) {
            for (size_t i = 0; i < s.count; ++i) {
                dirty |= update_pixel_effect_locked(i, EFFECT_TICK_MS);
            }
        }

        if (dirty) {
            esp_err_t err = render_locked();
            if (err != ESP_OK) {
                stop_all_effects_on_render_error_locked(err);
            }
        }

        unlock_ctx();
    }

    xSemaphoreGive(s.worker_stopped);
    vTaskDelete(NULL);
}

static void cleanup_init_failure(void)
{
    if (s.strip) {
        led_strip_del(s.strip);
    }
    if (s.worker_stopped) {
        vSemaphoreDelete(s.worker_stopped);
    }
    if (s.lock) {
        vSemaphoreDelete(s.lock);
    }
    free(s.pixels);
    memset(&s, 0, sizeof(s));
}

esp_err_t neopixel_init(const neopixel_config_t *cfg)
{
    if (!cfg ||
        cfg->led_count == 0 ||
        cfg->default_brightness > 100 ||
        cfg->format > NEOPIXEL_FORMAT_GRB ||
        !GPIO_IS_VALID_OUTPUT_GPIO(cfg->gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s, 0, sizeof(s));

    s.lock = xSemaphoreCreateMutex();
    s.worker_stopped = xSemaphoreCreateBinary();
    s.pixels = calloc(cfg->led_count, sizeof(*s.pixels));
    if (!s.lock || !s.worker_stopped || !s.pixels) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < cfg->led_count; ++i) {
        s.pixels[i].output_brightness = cfg->default_brightness;
    }

    led_color_component_format_t format =
        cfg->format == NEOPIXEL_FORMAT_RGB
            ? LED_STRIP_COLOR_COMPONENT_FMT_RGB
            : LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = cfg->gpio_num,
        .max_leds = cfg->led_count,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = format,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg,
                                             &rmt_cfg,
                                             &s.strip);
    if (err != ESP_OK) {
        cleanup_init_failure();
        return err;
    }

    err = led_strip_clear(s.strip);
    if (err != ESP_OK) {
        cleanup_init_failure();
        return err;
    }

    s.gpio = cfg->gpio_num;
    s.count = cfg->led_count;
    s.format = cfg->format;
    s.brightness = cfg->default_brightness;
    s.on = true;
    s.task_running = true;

    if (xTaskCreate(effect_task,
                    "neopixel_fx",
                    EFFECT_TASK_STACK,
                    NULL,
                    EFFECT_TASK_PRIO,
                    &s.effect_task) != pdPASS) {
        s.task_running = false;
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }

    s.initialized = true;
    return ESP_OK;
}

esp_err_t neopixel_deinit(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* init/deinit are lifecycle boundaries and must be externally serialized.
     * Runtime worker shutdown itself uses mutex + semaphore synchronization. */
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }
    s.task_running = false;
    unlock_ctx();

    if (xSemaphoreTake(s.worker_stopped,
                       pdMS_TO_TICKS(NEOPIXEL_WORKER_STOP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t clear_err = led_strip_clear(s.strip);
    led_strip_handle_t strip = s.strip;
    neopixel_pixel_state_t *pixels = s.pixels;
    SemaphoreHandle_t lock = s.lock;
    SemaphoreHandle_t worker_stopped = s.worker_stopped;

    s.initialized = false;
    s.effect_task = NULL;

    esp_err_t delete_err = led_strip_del(strip);
    free(pixels);
    vSemaphoreDelete(worker_stopped);
    vSemaphoreDelete(lock);
    memset(&s, 0, sizeof(s));

    if (clear_err != ESP_OK) {
        return clear_err;
    }
    return delete_err;
}

esp_err_t neopixel_show(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = render_locked();
    unlock_ctx();
    return err;
}

esp_err_t neopixel_clear(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    stop_strip_effect_locked();
    stop_all_pixel_effects_locked();
    fill_logical_locked(NEOPIXEL_COLOR_OFF);
    set_all_output_brightness_locked(s.brightness);
    s.on = true;
    esp_err_t err = led_strip_clear(s.strip);

    unlock_ctx();
    return err;
}

esp_err_t neopixel_set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s.count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    stop_strip_effect_locked();
    stop_pixel_effect_state_locked(&s.pixels[index]);
    s.pixels[index].color = (rgb_t){r, g, b};
    s.pixels[index].output_brightness = s.brightness;
    s.current_color = neopixel_rgb_to_hex(r, g, b);
    s.on = true;
    esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}

esp_err_t neopixel_set_pixel_hex(size_t index, uint32_t color)
{
    uint8_t r, g, b;
    neopixel_hex_to_rgb(color, &r, &g, &b);
    return neopixel_set_pixel(index, r, g, b);
}

esp_err_t neopixel_clear_pixel(size_t index)
{
    return neopixel_set_pixel(index, 0, 0, 0);
}

esp_err_t neopixel_fill(uint8_t r, uint8_t g, uint8_t b)
{
    return neopixel_fill_hex(neopixel_rgb_to_hex(r, g, b));
}

esp_err_t neopixel_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    return neopixel_fill(r, g, b);
}

esp_err_t neopixel_set_color_hex(uint32_t color)
{
    return neopixel_fill_hex(color);
}

esp_err_t neopixel_set_static_state(uint32_t color,
                                    uint8_t brightness,
                                    bool power_on)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    stop_strip_effect_locked();
    stop_all_pixel_effects_locked();
    fill_logical_locked(color);
    s.brightness = brightness;
    set_all_output_brightness_locked(brightness);
    s.on = power_on;

    const esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}

esp_err_t neopixel_fill_hex(uint32_t color)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    stop_strip_effect_locked();
    stop_all_pixel_effects_locked();
    fill_logical_locked(color);
    set_all_output_brightness_locked(s.brightness);
    s.on = true;
    esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}

esp_err_t neopixel_set_brightness(uint8_t percent)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    s.brightness = percent;
    for (size_t i = 0; i < s.count; ++i) {
        if (s.pixels[i].effect == NEOPIXEL_EFFECT_NONE) {
            s.pixels[i].output_brightness = percent;
        }
    }

    esp_err_t err = s.effect == NEOPIXEL_EFFECT_NONE
                        ? render_locked()
                        : ESP_OK;

    unlock_ctx();
    return err;
}

uint8_t neopixel_get_brightness(void)
{
    uint8_t brightness = 0;
    if (!runtime_ready() || !lock_ctx()) {
        return brightness;
    }

    brightness = s.brightness;
    unlock_ctx();
    return brightness;
}

static void pause_effects_for_off_locked(void)
{
    if (s.effect != NEOPIXEL_EFFECT_NONE && !s.effect_paused) {
        s.effect_paused = true;
        s.effect_paused_by_off = true;
    }

    for (size_t i = 0; i < s.count; ++i) {
        neopixel_pixel_state_t *pixel = &s.pixels[i];
        if (pixel->effect != NEOPIXEL_EFFECT_NONE && !pixel->effect_paused) {
            pixel->effect_paused = true;
            pixel->effect_paused_by_off = true;
        }
    }
}

static void resume_effects_paused_by_off_locked(void)
{
    if (s.effect_paused_by_off) {
        s.effect_paused = false;
        s.effect_paused_by_off = false;
    }

    for (size_t i = 0; i < s.count; ++i) {
        neopixel_pixel_state_t *pixel = &s.pixels[i];
        if (pixel->effect_paused_by_off) {
            pixel->effect_paused = false;
            pixel->effect_paused_by_off = false;
        }
    }
}

esp_err_t neopixel_off(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    s.on = false;
    pause_effects_for_off_locked();
    esp_err_t err = led_strip_clear(s.strip);

    unlock_ctx();
    return err;
}

esp_err_t neopixel_on(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    s.on = true;
    resume_effects_paused_by_off_locked();
    esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}

esp_err_t neopixel_toggle(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err;
    if (s.on) {
        s.on = false;
        pause_effects_for_off_locked();
        err = led_strip_clear(s.strip);
    } else {
        s.on = true;
        resume_effects_paused_by_off_locked();
        err = render_locked();
    }

    unlock_ctx();
    return err;
}

bool neopixel_is_on(void)
{
    bool on = false;
    if (!runtime_ready() || !lock_ctx()) {
        return false;
    }

    on = s.on;
    unlock_ctx();
    return on;
}

uint32_t neopixel_get_color(void)
{
    uint32_t color = NEOPIXEL_COLOR_OFF;
    if (!runtime_ready() || !lock_ctx()) {
        return color;
    }

    color = s.current_color;
    unlock_ctx();
    return color;
}

neopixel_effect_t neopixel_get_effect(void)
{
    neopixel_effect_t effect = NEOPIXEL_EFFECT_NONE;
    if (!runtime_ready() || !lock_ctx()) {
        return effect;
    }

    effect = s.effect;
    unlock_ctx();
    return effect;
}

esp_err_t neopixel_start_effect(neopixel_effect_t effect,
                                const neopixel_effect_config_t *cfg)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t validation = validate_effect_config(effect, cfg, false);
    if (validation != ESP_OK) {
        return validation;
    }

    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    stop_all_pixel_effects_locked();
    stop_strip_effect_locked();
    s.effect = effect;
    s.effect_cfg = *cfg;
    s.first_effect_update = true;
    s.effect_paused = false;
    s.effect_paused_by_off = false;
    s.on = true;

    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_stop_effect(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    /* Stop means freeze the current logical/output frame; it does not restore
     * the state that existed before the effect started. */
    stop_strip_effect_locked();
    esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}

esp_err_t neopixel_pause_effect(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }
    if (s.effect == NEOPIXEL_EFFECT_NONE) {
        unlock_ctx();
        return ESP_ERR_INVALID_STATE;
    }

    s.effect_paused = true;
    s.effect_paused_by_off = false;
    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_resume_effect(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }
    if (s.effect == NEOPIXEL_EFFECT_NONE) {
        unlock_ctx();
        return ESP_ERR_INVALID_STATE;
    }

    s.effect_paused = false;
    s.effect_paused_by_off = false;
    s.on = true;
    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_set_pixel_effect(size_t index,
                                     neopixel_effect_t effect,
                                     const neopixel_effect_config_t *cfg)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s.count) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t validation = validate_effect_config(effect, cfg, true);
    if (validation != ESP_OK) {
        return validation;
    }

    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    stop_strip_effect_locked();

    neopixel_pixel_state_t *pixel = &s.pixels[index];
    stop_pixel_effect_state_locked(pixel);
    pixel->effect = effect;
    pixel->effect_cfg = *cfg;
    pixel->first_effect_update = true;
    pixel->effect_paused = false;
    pixel->effect_paused_by_off = false;
    s.on = true;

    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_stop_pixel_effect(size_t index)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s.count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    /* Freeze only the target pixel at its current color/output brightness. */
    stop_pixel_effect_state_locked(&s.pixels[index]);
    esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}

esp_err_t neopixel_pause_pixel_effect(size_t index)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s.count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    neopixel_pixel_state_t *pixel = &s.pixels[index];
    if (pixel->effect == NEOPIXEL_EFFECT_NONE) {
        unlock_ctx();
        return ESP_ERR_INVALID_STATE;
    }

    pixel->effect_paused = true;
    pixel->effect_paused_by_off = false;
    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_resume_pixel_effect(size_t index)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s.count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    neopixel_pixel_state_t *pixel = &s.pixels[index];
    if (pixel->effect == NEOPIXEL_EFFECT_NONE) {
        unlock_ctx();
        return ESP_ERR_INVALID_STATE;
    }

    pixel->effect_paused = false;
    pixel->effect_paused_by_off = false;
    s.on = true;
    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_stop_all_pixel_effects(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    /* Freeze each independently animated pixel at its current frame. */
    stop_all_pixel_effects_locked();
    esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}

esp_err_t neopixel_get_pixel_effect(size_t index,
                                    neopixel_effect_t *out_effect)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s.count || !out_effect) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    *out_effect = s.pixels[index].effect;
    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_get_pixel_color(size_t index, uint32_t *out_color)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= s.count || !out_color) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    *out_color = get_pixel_color_locked(index);
    unlock_ctx();
    return ESP_OK;
}

esp_err_t neopixel_blink(uint32_t color, uint8_t brightness,
                         uint32_t on_time_ms, uint32_t off_time_ms)
{
    neopixel_effect_config_t cfg = {
        .color = color,
        .brightness = brightness,
        .on_time_ms = on_time_ms,
        .off_time_ms = off_time_ms,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_BLINK, &cfg);
}

esp_err_t neopixel_fade_in(uint32_t color, uint32_t duration_ms)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t target = neopixel_get_brightness();
    return neopixel_fade(color, 0, target, duration_ms);
}

esp_err_t neopixel_fade_out(uint32_t duration_ms)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t color = neopixel_get_color();
    uint8_t brightness = neopixel_get_brightness();
    return neopixel_fade(color, brightness, 0, duration_ms);
}

esp_err_t neopixel_fade(uint32_t color, uint8_t from, uint8_t to,
                        uint32_t duration_ms)
{
    neopixel_effect_config_t cfg = {
        .color = color,
        .brightness_from = from,
        .brightness_to = to,
        .duration_ms = duration_ms,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_FADE, &cfg);
}

esp_err_t neopixel_transition(uint32_t from_color, uint32_t to_color,
                              uint32_t duration_ms)
{
    neopixel_effect_config_t cfg = {
        .color = from_color,
        .color_to = to_color,
        .duration_ms = duration_ms,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_TRANSITION, &cfg);
}

esp_err_t neopixel_rainbow(uint32_t duration_ms)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = 1;
    if (lock_ctx()) {
        count = s.count;
        unlock_ctx();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    neopixel_effect_config_t cfg = {
        .duration_ms = duration_ms,
        .hue_offset = count > 1 ? (uint16_t)(360u / count) : 0,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_RAINBOW, &cfg);
}

esp_err_t neopixel_rainbow_cycle(uint32_t speed_ms)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t count = 1;
    if (lock_ctx()) {
        count = s.count;
        unlock_ctx();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    neopixel_effect_config_t cfg = {
        .speed_ms = speed_ms,
        .hue_offset = count > 1 ? (uint16_t)(360u / count) : 0,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_RAINBOW_CYCLE, &cfg);
}

esp_err_t neopixel_breath(uint32_t color, uint32_t period_ms)
{
    neopixel_effect_config_t cfg = {
        .color = color,
        .duration_ms = period_ms,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_BREATH, &cfg);
}

esp_err_t neopixel_pulse(uint32_t color, uint8_t brightness,
                         uint32_t duration_ms, uint32_t count)
{
    neopixel_effect_config_t cfg = {
        .color = color,
        .brightness = brightness,
        .duration_ms = duration_ms,
        .count = count,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_PULSE, &cfg);
}

esp_err_t neopixel_chase(uint32_t color, uint32_t speed_ms)
{
    neopixel_effect_config_t cfg = {
        .color = color,
        .speed_ms = speed_ms,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_CHASE, &cfg);
}

esp_err_t neopixel_color_wipe(uint32_t color, uint32_t speed_ms)
{
    neopixel_effect_config_t cfg = {
        .color = color,
        .speed_ms = speed_ms,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_COLOR_WIPE, &cfg);
}

esp_err_t neopixel_theater_chase(uint32_t color, uint32_t speed_ms)
{
    neopixel_effect_config_t cfg = {
        .color = color,
        .speed_ms = speed_ms,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_THEATER_CHASE, &cfg);
}

esp_err_t neopixel_gradient(uint32_t start, uint32_t end)
{
    neopixel_effect_config_t cfg = {
        .color = start,
        .color_to = end,
    };
    return neopixel_start_effect(NEOPIXEL_EFFECT_GRADIENT, &cfg);
}

esp_err_t neopixel_rainbow_gradient(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lock_ctx()) {
        return ESP_ERR_TIMEOUT;
    }

    stop_strip_effect_locked();
    stop_all_pixel_effects_locked();

    for (size_t i = 0; i < s.count; ++i) {
        float hue = s.count <= 1
                        ? 0.0f
                        : 360.0f * (float)i / (float)s.count;
        set_pixel_color_locked(i,
                               neopixel_hsv_to_rgb(hue, 1.0f, 1.0f));
        s.pixels[i].output_brightness = s.brightness;
    }

    s.current_color = s.count > 0
                          ? get_pixel_color_locked(0)
                          : NEOPIXEL_COLOR_OFF;
    s.on = true;
    esp_err_t err = render_locked();

    unlock_ctx();
    return err;
}
