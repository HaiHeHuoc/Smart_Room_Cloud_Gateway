# common Component Notes

## Purpose

`common` stores shared project-level definitions that other components can include.
At the moment, it provides application identity through `app_common.h` and
hardware configuration through `board_config.h`.

This component should stay small. It should contain shared constants, common
types, and board/device configuration, not feature logic.

## What Is Done

- Provides LCD configuration for the ST7735 SPI TFT.
- Provides SD card SPI configuration.
- Provides `APP_PROJECT_NAME`, `APP_PROJECT_VER`, and
  `APP_PROJECT_VER_DATE`.
- Exposes GPIO pin mapping through one shared board config header.
- Exposes display resolution, LCD SPI speed, backlight level, transfer size,
  and LVGL draw buffer line count.
- Exposes SD card mount point, SPI host, SPI speed, max open files, and FAT
  allocation unit size.
- Exposes shared SD path buffer length through `SD_CARD_MANAGER_PATH_MAX_LEN`.

## How To Use

Include the board config header from any component that needs hardware mapping:

```c
#include "board_config.h"
```

Include the application metadata header when reporting firmware identity:

```c
#include "app_common.h"

ESP_LOGI(TAG, "%s v%s", APP_PROJECT_NAME, APP_PROJECT_VER);
```

Typical users:

- `display_driver` uses LCD SPI pins, resolution, backlight GPIO, and transfer
  size.
- `ui_manager_lvgl` uses LCD resolution and `LCD_LVGL_DRAW_BUF_LINES`.
- `sd_card_manager` uses SD SPI pins, mount point, clock, and FAT settings.
- `lvgl_sd_fs` uses the SD mount point and SD path length when mapping LVGL
  paths such as `S:/image.png` to VFS paths such as `/sdcard/image.png`.

## Important Notes

- `LCD_SPI_HOST` and `SD_SPI_HOST` are different. LCD uses `SPI2_HOST`; SD card
  uses `SPI3_HOST`.
- `LCD_LVGL_DRAW_BUF_LINES` is not the screen height. It is the number of rows
  LVGL renders per partial draw buffer.
- `SD_CLOCK_KHZ` is in kHz because ESP-IDF SDSPI host config expects kHz.
- `SD_CARD_MANAGER_PATH_MAX_LEN` is shared by SD file listing code and the LVGL
  SD filesystem adapter.
- Keep hardware pin changes here instead of spreading pin numbers across
  drivers.
- `APP_PROJECT_VER_DATE` is a string constant; it is not generated from the
  build time.

## Future Attention

- If more devices are added, keep each device section separated in
  `board_config.h`.
- Consider adding comments for any shared pins or SPI bus conflicts.
- If board variants appear later, split config by board revision or use
  menuconfig options.
- Avoid adding Wi-Fi, BLE, cloud, or application workflow logic here.
