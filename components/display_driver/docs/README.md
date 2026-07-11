# display_driver Component Notes

## Purpose

`display_driver` owns the low-level LCD bring-up path.
It prepares the LCD backlight GPIO, initializes the SPI bus, creates the ESP-IDF
LCD panel IO object, creates the ST7735 panel object, and exposes drawing helpers
for higher-level UI code.

LVGL should not talk to the ST7735 driver directly. LVGL should go through this
component via `display_driver_draw_bitmap()`.

## Public API

| API | Role |
| --- | --- |
| `display_driver_init()` | Initialize backlight GPIO, LCD SPI bus, panel IO, and ST7735 panel. |
| `display_driver_set_backlight()` | Turn the configured backlight GPIO on or off. |
| `display_driver_raw_color_test()` | Fill the screen with RGB565 test colors for bring-up. |
| `display_driver_draw_bitmap()` | Submit a rectangular RGB565 bitmap to the panel. |

## What Is Done

- Initializes the LCD backlight GPIO.
- Initializes the LCD SPI bus using settings from `board_config.h`.
- Creates ESP-IDF SPI panel IO with LCD CS/DC pins.
- Creates the ST7735 panel using `esp_lcd_new_panel_st7735()`.
- Resets, initializes, turns on, and enables the LCD backlight.
- Provides a raw color test helper for LCD bring-up.
- Provides a bitmap draw helper used by `ui_manager_lvgl`.

## How To Use

Create a display handle and initialize the driver:

```c
display_driver_handle_t display_handle = {0};

esp_err_t ret = display_driver_init(&display_handle);
if (ret != ESP_OK) {
    return;
}
```

Turn the backlight on or off:

```c
display_driver_set_backlight(true);
display_driver_set_backlight(false);
```

Draw bitmap data:

```c
display_driver_draw_bitmap(&display_handle, x1, y1, x2, y2, color_data);
```

## Important Notes

- Pixel format is RGB565, 16 bits per pixel.
- The draw buffer passed to SPI should be DMA-capable when used for large
  transfers.
- The driver currently swaps RGB565 byte order for raw color testing.
- `display_driver_draw_bitmap()` expects `x_end` and `y_end` to be exclusive,
  matching ESP-IDF LCD panel API style.
- SPI color transfers are asynchronous. `ui_manager_lvgl` registers the panel
  IO completion callback and waits for it before LVGL reuses its draw buffer.
- The LCD SPI bus is initialized inside this component. Do not initialize the
  same LCD SPI host somewhere else.
- The current implementation has no deinit path and is not designed for a
  second call to `display_driver_init()`.

## Future Attention

- Add a deinit function if the project later needs display shutdown or power
  management.
- Clean up duplicate includes in `display_driver.c`.
- Consider guarding SPI bus initialization if the display driver can be called
  more than once.
- Keep LVGL-specific logic out of this component; LVGL belongs in
  `ui_manager_lvgl`.
