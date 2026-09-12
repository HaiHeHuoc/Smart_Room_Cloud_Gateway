# neopixel

Reusable ESP-IDF WS2812/NeoPixel RGB component with non-blocking whole-strip and independent per-pixel effects.

## Compatibility

```text
Component version: 1.0.0
ESP-IDF:           >= 6.0
led_strip:         ^3.0.3
LED formats:       RGB, GRB
Backend:           RMT
```

## Dependency

A consuming ESP-IDF component can depend on `neopixel` directly:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES neopixel
)
```

Include:

```c
#include "neopixel.h"
```

## Minimal Initialization

```c
const neopixel_config_t config = {
    .gpio_num = GPIO_NUM_48,
    .led_count = 1,
    .format = NEOPIXEL_FORMAT_GRB,
    .default_brightness = 30,
};

ESP_ERROR_CHECK(neopixel_init(&config));
```

## Static Control

```c
ESP_ERROR_CHECK(neopixel_set_color_hex(NEOPIXEL_COLOR_RED));
ESP_ERROR_CHECK(neopixel_set_brightness(40));
ESP_ERROR_CHECK(neopixel_off());
ESP_ERROR_CHECK(neopixel_on());
```

Product-level wrappers that must update color, brightness, and power without
intermediate rendered frames can use `neopixel_set_static_state()`. It stores
the requested logical RGB and brightness while OFF, stops active effects, and
renders exactly once.

Per-pixel:

```c
ESP_ERROR_CHECK(neopixel_set_pixel_hex(0, NEOPIXEL_COLOR_RED));
ESP_ERROR_CHECK(neopixel_clear_pixel(1));
```

## Effects

Whole strip:

```c
ESP_ERROR_CHECK(neopixel_blink(
    NEOPIXEL_COLOR_RED,
    50,
    500,
    500));

ESP_ERROR_CHECK(neopixel_breath(
    NEOPIXEL_COLOR_BLUE,
    2000));
```

One pixel only:

```c
neopixel_effect_config_t effect = {
    .color = NEOPIXEL_COLOR_BLUE,
    .duration_ms = 2000,
    .step_ms = 20,
};

ESP_ERROR_CHECK(neopixel_set_pixel_effect(
    3,
    NEOPIXEL_EFFECT_BREATH,
    &effect));
```

Per-pixel effects supported in v1.0:

- `SOLID`
- `BLINK`
- `FADE`
- `BREATH`
- `PULSE`
- `TRANSITION`
- `RAINBOW`
- `RAINBOW_CYCLE`

Strip-spatial effects such as `CHASE`, `COLOR_WIPE`, `GRADIENT`, and `THEATER_CHASE` return `ESP_ERR_NOT_SUPPORTED` when requested for one pixel.

## Ownership Rules

- Whole-strip setters/effects stop all per-pixel effects.
- A per-pixel setter/effect stops an active whole-strip effect.
- A per-pixel command replaces only the target pixel's effect; neighboring pixel effects continue.
- OFF pauses running effects while preserving state.
- ON resumes only effects paused by OFF.

## Stop Semantics

`neopixel_stop_effect()`, `neopixel_stop_pixel_effect()`, and `neopixel_stop_all_pixel_effects()` use **freeze semantics**.

They remove effect ownership while keeping the current logical color and output brightness. They do not restore the state that existed before the effect started.

## Concurrency and Lifecycle

Runtime APIs are serialized with a FreeRTOS mutex and are intended for normal task context only; they are not ISR-safe.

The effect engine uses one persistent worker task for all effects. `neopixel_deinit()` requests worker shutdown under the mutex and waits on an internal binary semaphore before releasing strip, pixel-buffer, mutex, and worker-synchronization resources.

`neopixel_init()` and `neopixel_deinit()` remain lifecycle boundaries and must not race with other public API calls.

## Full Documentation

See:

```text
docs/README.md
```

for architecture, API groups, effect semantics, examples, verification, and known limitations.

## Test Firmware

Repository test application:

```text
Test/neopixel/
```

Expected release-candidate result after local verification:

```text
5 Tests 0 Failures 0 Ignored
```

## Release Note

The component declares version `1.0.0`, but the branch should remain a release candidate until the ESP-IDF demo build and Unity tests pass locally. Multi-LED physical validation can be completed later when external strip hardware is available.
