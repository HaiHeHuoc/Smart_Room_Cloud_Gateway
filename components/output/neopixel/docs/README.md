# neopixel

## Purpose

`neopixel` is a reusable ESP-IDF component for WS2812-compatible RGB addressable LEDs. It wraps the Espressif `led_strip` RMT backend and provides:

- RGB and HEX color control.
- Per-pixel and whole-strip static operations.
- Global brightness without destroying logical RGB values.
- ON/OFF/toggle state preservation.
- Non-blocking whole-strip effects.
- Independent per-pixel effects running concurrently.
- Multi-LED strip effects such as chase, wipe, theater chase, gradient, and rainbow gradient.
- RGB/HEX/HSV color utilities.
- Mutex-protected runtime access for normal FreeRTOS task callers.
- Synchronized worker shutdown during deinitialization.
- `esp_err_t` error reporting for state-changing operations.

This README is the detailed design, usage, lifecycle, verification, and acceptance document for the component.

## Release Status

The implementation declares component version:

```text
1.0.0
```

The branch should still be treated as a **v1.0.0 release candidate** until these local checks pass:

```text
repository demo build
Test/neopixel build
5 Tests 0 Failures 0 Ignored
```

Multi-LED physical acceptance is explicitly deferred until external strip hardware is available. It is not treated as missing source implementation.

## Ownership Boundary

The component owns:

- NeoPixel configuration validation.
- Creation and deletion of the Espressif `led_strip` RMT device.
- Logical RGB state for every configured LED.
- Per-pixel physical-output brightness state.
- Global brightness policy for static pixels and effect maxima.
- ON/OFF state.
- Whole-strip effect state.
- Independent per-pixel effect state.
- One internal FreeRTOS effect worker task.
- Runtime serialization with one non-recursive mutex.
- Worker-shutdown synchronization with one binary semaphore.
- Effect replacement, pause, resume, stop, and ownership semantics.

The component does not:

- Own application status policy such as Wi-Fi/cloud/error color mapping.
- Call GUI, network, provisioning, cloud, or application callbacks.
- Support ISR callers.
- Manage external LED power supplies or level shifting.
- Support RGBW/GRBW in v1.0.
- Support multiple independent strips in v1.0.
- Guarantee physical behavior for LED families not compatible with the configured WS2812 timing/backend.

The application decides what each color/effect means and when it should be used.

## Component Structure

```text
components/neopixel/
|-- CMakeLists.txt
|-- idf_component.yml
|-- README.md
|-- neopixel.c
|-- include/
|   `-- neopixel.h
`-- docs/
    `-- README.md
```

Repository-level demo:

```text
main/
|-- CMakeLists.txt
`-- main.c
```

Isolated Unity test firmware:

```text
Test/neopixel/
|-- CMakeLists.txt
|-- README.md
`-- main/
    |-- CMakeLists.txt
    `-- test_neopixel.c
```

## Compatibility and Dependencies

The component manifest currently declares:

```yaml
version: "1.0.0"
dependencies:
  idf: ">=6.0"
  espressif/led_strip: "^3.0.3"
```

Supported v1.0 color order:

- `NEOPIXEL_FORMAT_RGB`
- `NEOPIXEL_FORMAT_GRB`

RGBW/GRBW are intentionally outside v1.0 because correct support requires an explicit logical white channel and matching public setter APIs.

The component CMake dependency boundary is:

```cmake
idf_component_register(
    SRCS "neopixel.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES led_strip freertos
)
```

`esp_driver_gpio` is public because `neopixel.h` exposes `gpio_num_t`. `led_strip` and FreeRTOS details remain implementation dependencies.

## Backend Architecture

```text
application tasks
       |
       v
neopixel public API
       |
       v
FreeRTOS mutex
       |
       v
logical strip/pixel state
       |
       +-----------------------+
       |                       |
       v                       v
whole-strip effect      per-pixel effects
       |                       |
       +-----------+-----------+
                   |
                   v
          one neopixel_fx task
                   |
                   v
          one render per dirty tick
                   |
                   v
          Espressif led_strip
                   |
                   v
                 RMT
                   |
                   v
          WS2812-compatible LEDs
```

The component does **not** create one task per pixel.

## Initialization

Example for one onboard GRB NeoPixel on GPIO48:

```c
const neopixel_config_t config = {
    .gpio_num = GPIO_NUM_48,
    .led_count = 1,
    .format = NEOPIXEL_FORMAT_GRB,
    .default_brightness = 30,
};

esp_err_t err = neopixel_init(&config);
```

Validation rules:

- configuration pointer must not be `NULL`;
- `led_count > 0`;
- brightness must be in `0..100`;
- format must be RGB or GRB;
- GPIO must be valid for output;
- repeated successful initialization returns `ESP_ERR_INVALID_STATE`.

Initialization allocates:

- logical per-pixel state array;
- runtime mutex;
- worker-stopped binary semaphore;
- `led_strip` RMT device;
- one persistent effect task.

Public runtime APIs must only be called after successful initialization.

## Lifecycle and Worker Shutdown

`neopixel_init()` and `neopixel_deinit()` are lifecycle boundaries. The application must not race either lifecycle operation with another public NeoPixel API.

Runtime effect shutdown itself is synchronized internally.

Deinitialization flow:

```text
neopixel_deinit()
        |
        v
lock component mutex
        |
        v
set task_running = false
        |
        v
unlock mutex
        |
        v
wait on worker_stopped semaphore
        |
        v
worker observes stop request under same mutex
        |
        v
worker gives worker_stopped semaphore
        |
        v
worker deletes itself
        |
        v
deinit releases strip/buffer/semaphores
```

This replaces the earlier polling-style shutdown based on repeatedly reading a shared `TaskHandle_t`.

The shutdown wait is bounded. A synchronization timeout returns `ESP_ERR_TIMEOUT` instead of waiting forever.

## Logical Color and Brightness Model

Logical RGB and physical-output brightness are separate state.

Example:

```c
neopixel_set_color_hex(NEOPIXEL_COLOR_RED);
neopixel_set_brightness(30);
```

Logical color remains full red:

```text
R = 255
G = 0
B = 0
```

Only physical output is scaled during render.

Static pixels normally use global brightness. Active effects may temporarily own per-pixel output brightness for effects such as:

- BLINK
- FADE
- BREATH
- PULSE

Changing global brightness updates static pixels immediately. Active per-pixel effects retain their temporary output brightness until their next effect update or until they stop.

## Public API Groups

### Lifecycle

- `neopixel_init()`
- `neopixel_deinit()`
- `neopixel_clear()`
- `neopixel_show()`

### Static color control

- `neopixel_set_color()`
- `neopixel_set_color_hex()`
- `neopixel_set_pixel()`
- `neopixel_set_pixel_hex()`
- `neopixel_clear_pixel()`
- `neopixel_fill()`
- `neopixel_fill_hex()`

### Brightness and global state

- `neopixel_set_brightness()`
- `neopixel_get_brightness()`
- `neopixel_on()`
- `neopixel_off()`
- `neopixel_toggle()`
- `neopixel_is_on()`
- `neopixel_get_color()`
- `neopixel_get_effect()`

### Whole-strip effects

- `neopixel_blink()`
- `neopixel_fade_in()`
- `neopixel_fade_out()`
- `neopixel_fade()`
- `neopixel_transition()`
- `neopixel_rainbow()`
- `neopixel_rainbow_cycle()`
- `neopixel_breath()`
- `neopixel_pulse()`
- `neopixel_chase()`
- `neopixel_color_wipe()`
- `neopixel_theater_chase()`
- `neopixel_gradient()`
- `neopixel_rainbow_gradient()`
- `neopixel_start_effect()`
- `neopixel_stop_effect()`
- `neopixel_pause_effect()`
- `neopixel_resume_effect()`

### Independent per-pixel effects

- `neopixel_set_pixel_effect()`
- `neopixel_stop_pixel_effect()`
- `neopixel_pause_pixel_effect()`
- `neopixel_resume_pixel_effect()`
- `neopixel_stop_all_pixel_effects()`
- `neopixel_get_pixel_effect()`
- `neopixel_get_pixel_color()`

### Color utilities

- `neopixel_rgb_to_hex()`
- `neopixel_hex_to_rgb()`
- `neopixel_hsv_to_rgb()`
- `neopixel_rgb_to_hsv()`
- `neopixel_color_lerp()`

Color utility functions do not require component initialization.

## Predefined Colors

```c
NEOPIXEL_COLOR_OFF
NEOPIXEL_COLOR_RED
NEOPIXEL_COLOR_GREEN
NEOPIXEL_COLOR_BLUE
NEOPIXEL_COLOR_WHITE
NEOPIXEL_COLOR_YELLOW
NEOPIXEL_COLOR_CYAN
NEOPIXEL_COLOR_MAGENTA
NEOPIXEL_COLOR_ORANGE
NEOPIXEL_COLOR_PURPLE
```

## Whole-Strip Effects

Convenience example:

```c
ESP_ERROR_CHECK(neopixel_blink(
    NEOPIXEL_COLOR_RED,
    50,
    500,
    500));
```

The public call returns after installing effect state. The worker task advances the animation asynchronously.

Generic example:

```c
neopixel_effect_config_t effect = {
    .color = NEOPIXEL_COLOR_BLUE,
    .brightness = 60,
    .on_time_ms = 250,
    .off_time_ms = 750,
    .step_ms = 50,
};

ESP_ERROR_CHECK(neopixel_start_effect(
    NEOPIXEL_EFFECT_BLINK,
    &effect));
```

The worker base tick is currently 20 ms. `step_ms` may request a slower update interval. Values below the base tick are effectively clamped to the base tick.

## Independent Per-Pixel Effect Model

Each LED owns independent effect state:

```text
pixel[i]
|-- current RGB
|-- current output brightness
|-- active effect
|-- effect config
|-- pause flags
`-- effect timing
```

Per-pixel supported effects:

- `NEOPIXEL_EFFECT_SOLID`
- `NEOPIXEL_EFFECT_BLINK`
- `NEOPIXEL_EFFECT_FADE`
- `NEOPIXEL_EFFECT_BREATH`
- `NEOPIXEL_EFFECT_PULSE`
- `NEOPIXEL_EFFECT_TRANSITION`
- `NEOPIXEL_EFFECT_RAINBOW`
- `NEOPIXEL_EFFECT_RAINBOW_CYCLE`

The following are strip-spatial and return `ESP_ERR_NOT_SUPPORTED` for one pixel:

- `NEOPIXEL_EFFECT_COLOR_WIPE`
- `NEOPIXEL_EFFECT_CHASE`
- `NEOPIXEL_EFFECT_GRADIENT`
- `NEOPIXEL_EFFECT_THEATER_CHASE`

## Mixed 10-LED Example

Required behavior:

```text
LED 1 -> fixed RED
LED 2 -> RAINBOW
LED 3 -> OFF
LED 4 -> BREATH BLUE
LED 5-10 -> independent existing/static state
```

The API uses zero-based indexes:

```c
/* LED 1 */
ESP_ERROR_CHECK(neopixel_set_pixel_hex(
    0,
    NEOPIXEL_COLOR_RED));

/* LED 2 */
neopixel_effect_config_t rainbow = {
    .speed_ms = 10,
    .step_ms = 20,
};

ESP_ERROR_CHECK(neopixel_set_pixel_effect(
    1,
    NEOPIXEL_EFFECT_RAINBOW_CYCLE,
    &rainbow));

/* LED 3 */
ESP_ERROR_CHECK(neopixel_clear_pixel(2));

/* LED 4 */
neopixel_effect_config_t breath = {
    .color = NEOPIXEL_COLOR_BLUE,
    .duration_ms = 2000,
    .step_ms = 20,
};

ESP_ERROR_CHECK(neopixel_set_pixel_effect(
    3,
    NEOPIXEL_EFFECT_BREATH,
    &breath));
```

All active per-pixel effects are updated by the same worker and rendered together once per dirty worker cycle.

## Ownership and Replacement Rules

Ownership is deterministic.

### Whole-strip command

Examples:

```c
neopixel_set_color_hex(...);
neopixel_fill_hex(...);
neopixel_blink(...);
neopixel_rainbow_cycle(...);
```

Whole-strip commands:

- stop the current whole-strip effect when replacing it;
- stop all per-pixel effects;
- take ownership of the complete strip.

### Per-pixel command

Examples:

```c
neopixel_set_pixel_hex(index, color);
neopixel_set_pixel_effect(index, effect, &config);
```

Per-pixel commands:

- stop any active whole-strip effect;
- stop/replace only the effect owned by the addressed pixel;
- preserve effects running on all neighboring pixels.

This prevents simultaneous whole-strip and per-pixel writers from fighting over the same logical frame.

## Stop Effect Semantics

v1.0 uses **freeze semantics**.

Calling:

```c
neopixel_stop_effect();
```

means:

```text
remove whole-strip effect ownership
-> preserve the current logical colors
-> preserve current per-pixel output brightness
-> render that frozen frame
```

It does **not** restore the state that existed before the effect started.

The same rule applies to:

```c
neopixel_stop_pixel_effect(index);
neopixel_stop_all_pixel_effects();
```

For example, stopping BLINK while its current frame is dark leaves that frame dark until a later command changes color/brightness/state.

This behavior is intentionally simple and deterministic. A future restore-on-stop design would require explicit pre-effect snapshots and is outside v1.0.

## ON/OFF Semantics

`neopixel_off()`:

- clears physical LED output;
- preserves logical RGB values;
- preserves global brightness;
- pauses the whole-strip effect if it was running;
- pauses every running per-pixel effect.

`neopixel_on()`:

- restores the current logical/output frame;
- resumes only effects that were paused specifically by OFF.

An effect explicitly paused with:

```c
neopixel_pause_effect();
neopixel_pause_pixel_effect(index);
```

remains paused across OFF/ON until explicitly resumed.

## Finite Effect Completion Semantics

- `FADE`: final logical color remains the fade color; whole-strip fade commits global brightness to `brightness_to`.
- Per-pixel `FADE`: commits the target pixel's final output brightness but does not change global brightness.
- `TRANSITION`: final logical color becomes `color_to`.
- `PULSE`: finishes after the requested count and leaves output brightness at zero while preserving the pulse color logically.
- finite `RAINBOW`: stops after its duration.
- `GRADIENT`: renders once and stops.
- `SOLID`: renders once and stops; whole-strip SOLID commits its configured brightness.

Continuous effects include BLINK, BREATH, RAINBOW_CYCLE, CHASE, COLOR_WIPE, and THEATER_CHASE.

## Thread Safety

Normal runtime APIs serialize shared state and hardware access with one FreeRTOS mutex.

Protected state includes:

- logical pixel colors;
- per-pixel output brightness;
- global brightness;
- ON/OFF state;
- whole-strip effect state/config/timing;
- per-pixel effect state/config/timing;
- physical `led_strip` writes;
- worker stop request.

Rules:

- runtime APIs may be called from different normal FreeRTOS tasks;
- APIs are not ISR-safe;
- public APIs do not intentionally sleep while holding the component mutex;
- lock acquisition is bounded;
- lifecycle operations must be externally serialized against other public APIs.

## Error Handling

State-changing APIs primarily return:

- `ESP_OK`
- `ESP_ERR_INVALID_ARG`
- `ESP_ERR_INVALID_STATE`
- `ESP_ERR_NOT_SUPPORTED`
- `ESP_ERR_NO_MEM`
- `ESP_ERR_TIMEOUT`
- underlying `led_strip`/RMT errors

Reusable component code does not call `ESP_ERROR_CHECK()` internally. The application owns fatal/retry policy.

If the worker encounters a render error, it logs the failure and stops whole-strip and per-pixel effect ownership instead of silently continuing the animations.

## Main Demo Application

Current repository demo configuration:

```text
GPIO:       48
LED count:  1
Format:     GRB
Brightness: 30%
```

Build:

```powershell
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

The demo covers the original single-LED color/effect flow. It is intentionally not changed into a fake physical multi-LED demo.

## Test Firmware

Build:

```powershell
cd Test/neopixel
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Release-candidate suite:

```text
5 Tests
```

Coverage includes:

1. Color utilities.
2. Invalid-state contract.
3. Single-LED runtime and freeze semantics.
4. Independent multi-pixel software state/ownership.
5. Repeated worker shutdown/re-init while an effect is active.

Expected final acceptance result:

```text
5 Tests 0 Failures 0 Ignored
```

The exact result remains pending until the branch is built and run locally with ESP-IDF.

## Verification Status

Implemented and statically reviewed:

- [x] RGB/GRB configuration.
- [x] Static strip and per-pixel control.
- [x] Global brightness/state management.
- [x] Whole-strip effects.
- [x] Independent per-pixel effects.
- [x] One worker task for all effects.
- [x] One render after all due per-pixel updates in a worker cycle.
- [x] Mutex-protected runtime state.
- [x] Binary-semaphore worker shutdown handshake.
- [x] Bounded shutdown wait.
- [x] Freeze semantics documented in header and implementation.
- [x] Package version/description/IDF dependency metadata.
- [x] Repository and component README entry points.
- [x] Unity lifecycle test added.

Pending local release verification:

- [ ] Repository/demo `idf.py build`.
- [ ] `Test/neopixel` `idf.py build`.
- [ ] `5 Tests 0 Failures 0 Ignored`.
- [ ] Single onboard GPIO48 LED smoke test after hardening changes.

Pending later multi-LED physical acceptance:

- [ ] LED 1 static while LED 2 runs rainbow.
- [ ] LED 3 OFF while LED 4 breathes blue.
- [ ] Three or more concurrent independent pixel effects.
- [ ] Per-pixel pause/resume visibly affects only the target LED.
- [ ] Neighboring effects survive direct target-pixel changes.
- [ ] Whole-strip takeover affects all physical LEDs.
- [ ] Chase/wipe/theater-chase/gradient physical distribution.
- [ ] 10-30 minute mixed-effect stress run.

## Known Limitations

- Singleton component: only one strip instance at a time.
- RGBW/GRBW not supported in v1.0.
- Per-pixel spatial effects are intentionally unsupported.
- Effect timing is FreeRTOS-task scheduling, not a hard-real-time animation scheduler.
- `neopixel_get_color()` cannot describe every pixel of a mixed strip; use `neopixel_get_pixel_color()`.
- Gamma correction and color-temperature correction are not implemented.
- Multi-LED physical acceptance remains pending until suitable hardware exists.
- A public/distribution license has not yet been selected.

## Package and Release Boundary

The source/package structure is ready for reusable local ESP-IDF component consumption.

Before tagging `v1.0.0`:

```text
1. build repository demo
2. build Test/neopixel
3. run Unity suite
4. confirm 5/5 pass
5. smoke-test GPIO48
6. tag v1.0.0
```

A license must be selected before public distribution or public component-registry publication.

No additional visual effects are required for v1.0. New features should only be added when a real product or hardware requirement justifies expanding the API/test surface.
