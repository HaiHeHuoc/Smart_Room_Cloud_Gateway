# waveshare__esp_lcd_st7735 Component Notes

## Purpose

`waveshare__esp_lcd_st7735` provides the ST7735 LCD panel driver used by
`display_driver`.
It adapts the ST7735 command sequence to ESP-IDF's `esp_lcd_panel` interface.

This is a local component copy, so project-specific compatibility fixes can be
kept here without editing managed component cache files.

## What Is Done

- Provides `esp_lcd_new_panel_st7735()`.
- Implements ST7735 panel operations such as reset, init, draw bitmap, invert,
  mirror, swap XY, gap, display on/off, and delete.
- Supports vendor-specific initialization commands through `vendor_config`.
- Uses `rgb_ele_order` from `esp_lcd_panel_dev_config_t`, which matches newer
  ESP-IDF versions.
- Returns errors from `esp_lcd_panel_io_tx_color()` instead of silently ignoring
  failed color transfers.

## How To Use

This component is normally used by `display_driver`, not directly by application
code.

Basic panel creation flow:

```c
esp_lcd_panel_dev_config_t panel_config = {
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    .bits_per_pixel = 16,
    .reset_gpio_num = LCD_GPIO_RST,
};

esp_lcd_new_panel_st7735(io_handle, &panel_config, &panel_handle);
```

Then use ESP-IDF panel APIs:

```c
esp_lcd_panel_reset(panel_handle);
esp_lcd_panel_init(panel_handle);
esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2, y2, color_data);
```

## Important Notes

- Older ST7735 code may use `rgb_endian`. ESP-IDF v6 uses
  `rgb_ele_order` and `LCD_RGB_ELEMENT_ORDER_RGB/BGR`.
- `esp_lcd_panel_io_tx_color()` is asynchronous for SPI LCD IO. LVGL must be
  notified after the color transfer completes.
- The draw bitmap function must return transfer errors. If it always returns
  `ESP_OK`, LVGL can wait for a transfer-complete callback that never happens.
- Panel offsets and color order depend on the ST7735 tab variant.

## Future Attention

- Keep this local copy in sync with ESP-IDF API changes.
- Confirm the correct ST7735 init sequence for the exact LCD tab/color variant.
- Document any required X/Y gap offsets if the visible area is shifted.
- Avoid modifying this component for LVGL behavior; LVGL coordination belongs in
  `ui_manager_lvgl`.
