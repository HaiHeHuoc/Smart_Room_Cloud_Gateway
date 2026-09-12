# light_manager

`light_manager` is the Smart Room product-level owner for static
WS2812-compatible NeoPixel state. It accepts board configuration at runtime,
owns the copyable product state, and delegates RMT/LED-strip details to the
reusable sibling `neopixel` component.

## Ownership and dependencies

```text
main composition -> light_manager -> neopixel -> led_strip/RMT -> GPIO
```

Only `light_manager` exposes application-facing light control. It does not
depend on `board_config.h`, GUI, Xiaozhi/MCP, Wi-Fi, cloud, NVS, or application
coordinators. `neopixel` owns the device-driver handle, pixel buffer, RMT
backend, and optional effect worker. This manager deliberately exposes only
static power/RGB/brightness control.

## Configuration and lifecycle

The composition root supplies the physical mapping. The current Smart Room
composition is in `main/main.c`; it passes the board-owned `NEOPIXEL_GPIO` and
`NEOPIXEL_LED_COUNT` constants from `board_config.h`.

```c
const light_manager_config_t config = {
    .gpio_num = NEOPIXEL_GPIO,
    .led_count = NEOPIXEL_LED_COUNT,
    .pixel_format = LIGHT_MANAGER_PIXEL_FORMAT_GRB,
    .default_brightness_percent = 100U,
};

ESP_ERROR_CHECK(light_manager_init(&config));
```

`light_manager_init()` validates its input, initializes the lower layer, and
always finishes with the physical LEDs OFF. The initial logical color is black
and the configured default brightness is retained for future requests.
`light_manager_deinit()` turns the LEDs OFF through the lower layer, releases
that driver, and clears product state. Calling init twice, deinit before init,
or a runtime control API before init returns `ESP_ERR_INVALID_STATE`.

## Static API and state semantics

`light_manager_set_state()` applies power, RGB, and brightness as one logical
operation. The lower layer receives an atomic static-state update and renders
once, avoiding temporary frames from a sequence of unrelated setters.

```c
const light_manager_state_t magenta = {
    .power_on = true,
    .red = 255U,
    .green = 0U,
    .blue = 255U,
    .brightness_percent = 100U,
};

ESP_ERROR_CHECK(light_manager_set_state(&magenta));
```

- `light_manager_set_color()` and `light_manager_set_brightness()` preserve
  other fields.
- `light_manager_off()` preserves RGB and brightness. A following
  `light_manager_on()` restores that stored state.
- Brightness is an inclusive `0..100` percentage. `0` is valid and produces
  zero light output while preserving RGB and the ON logical state.
- Brightness above `100` and NULL state/output pointers return
  `ESP_ERR_INVALID_ARG`; values are never silently clamped.
- `light_manager_get_state()` copies only the logical state. It exposes no
  driver, RMT, LED-strip, or mutex handle.

If a lower-layer render call fails, the error is returned and the manager keeps
its previously confirmed logical state; callers may retry the full requested
state. Physical output must be considered unknown until a successful retry.

## Concurrency and context

Public calls are serialized by an internal static FreeRTOS mutex. The manager
uses no worker task. It is safe to call these APIs from normal FreeRTOS task
context; it is not ISR-safe. Lifecycle and runtime calls use the same mutex, so
deinit does not race an in-flight manager operation. The static synchronization
object remains valid across deinit/reinit; all NeoPixel resources are released
by the underlying component.

The lower NeoPixel library also serializes its own state and owns its optional
effect worker. `light_manager` does not expose effects because the Phase-18
product contract is static light control only.

## Known limitations

- One singleton strip is supported because the reused `neopixel` component is
  singleton-based.
- RGB and GRB three-channel LEDs are supported; RGBW/GRBW, calibration, gamma
  correction, and named-color parsing are intentionally out of scope.
- Natural-language color mapping and MCP registration belong to the future
  command layer, not this manager.
- Hardware confirmation still requires the target-board HIL sequence.
