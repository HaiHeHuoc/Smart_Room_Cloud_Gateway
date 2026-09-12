# light_manager

`light_manager` is the Smart Room product-level owner for bounded
WS2812-compatible NeoPixel state and effects. It accepts board configuration at
runtime, owns the copyable product state, and delegates RMT/LED-strip details to
the reusable sibling `neopixel` component.

## Ownership and dependencies

```text
main composition -> light_manager -> neopixel -> led_strip/RMT -> GPIO
```

Only `light_manager` exposes application-facing light control. It does not
depend on `board_config.h`, GUI, Xiaozhi/MCP, Wi-Fi, cloud, NVS, or application
coordinators. `neopixel` owns the device-driver handle, pixel buffer, RMT
backend, and optional effect worker. This manager deliberately exposes only
power/RGB/brightness/effect control.

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

## Product API and state semantics

`light_manager_set_state()` applies power, RGB, brightness, and one bounded
product effect as one logical operation. The public effect set is deliberately
small: `solid`, `blink`, `breath`, `pulse`, and `rainbow`; it never exposes the
lower-level NeoPixel effect enum or timing/configuration knobs.

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
  other fields, including the selected effect.
- `solid` renders stored RGB at stored brightness. Switching from an effect to
  `solid` stops that effect and restores the logical static output.
- `blink` uses fixed 500 ms ON / 500 ms OFF timing; `breath` uses a fixed
  2000 ms period; `pulse` repeats a fixed 1200 ms triangular pulse; `rainbow`
  uses the lower-layer single-LED rainbow-cycle behavior. No public API accepts
  arbitrary effect timing or raw NeoPixel configuration.
- `light_manager_off()` preserves RGB, brightness, and effect while darkening
  the LED. A following `light_manager_on()` resumes the same effect when one
  was active, or restores the stored solid state.
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
effect worker. `light_manager` translates only the five product effects above
to lower-layer operations; MCP and other application components do not access
that worker or lower-level effects directly.

## Target test loop

`CONFIG_LIGHT_MANAGER_TEST_LOOP` defaults to `n`. When explicitly enabled in
`menuconfig`, `main` starts the infinite `light_test` task immediately after a
successful `light_manager_init()`. It writes `LIGHT_TEST` serial markers and
waits `CONFIG_LIGHT_MANAGER_TEST_STEP_DELAY_MS` (default: 8000 ms) after every
operation.

One loop exercises the five public product effects through
`light_manager_set_state()` and logs the copied state after each request. It
then verifies the product transition from an effect to `solid` and the
OFF/ON preservation and resumption path for `blink`. The loop does not expose
or independently test lower-level NeoPixel-only effects; `main` never accesses
`neopixel` directly.

The test loop is intentionally non-terminating. While it is enabled and
running, `light_manager_deinit()` returns `ESP_ERR_INVALID_STATE`; flash a
normal configuration with the option disabled before production use. With the
current one-LED board, `rainbow` is visual acceptance of its single-LED cycle,
not proof of a multi-LED pattern.

## Known limitations

- One singleton strip is supported because the reused `neopixel` component is
  singleton-based.
- RGB and GRB three-channel LEDs are supported; RGBW/GRBW, calibration, gamma
  correction, and named-color parsing are intentionally out of scope.
- Natural-language color mapping and MCP registration belong to the product
  command layer, not this manager.
- Hardware confirmation still requires the target-board HIL sequence.
