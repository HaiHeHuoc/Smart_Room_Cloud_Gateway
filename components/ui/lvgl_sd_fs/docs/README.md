# lvgl_sd_fs Component Notes

## Purpose

`lvgl_sd_fs` connects LVGL's filesystem interface to the SD card mounted by
`sd_card_manager`.

After this component is registered, LVGL can load file-backed assets from the SD
card using an LVGL drive-letter path such as:

```text
S:/images/logo.png
S:/gif/demo.gif
S:/fonts/my_font.bin
```

Internally, those paths are mapped to the ESP-IDF VFS mount point:

```text
/sdcard/images/logo.png
/sdcard/gif/demo.gif
/sdcard/fonts/my_font.bin
```

## What Is Done

- Registers an LVGL filesystem driver with drive letter `S`.
- Checks SD card availability through `sd_card_manager_is_mounted()`.
- Maps LVGL paths to `SD_MOUNT_POINT`.
- Opens files using standard C `fopen()`.
- Supports LVGL file close, read, seek, and tell callbacks.
- Exposes registration and readiness state helpers.
- Keeps LVGL filesystem cache disabled to reduce RAM use during bring-up.

## How To Use

Initialize LVGL and mount the SD card before registering this component.
One valid order is:

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(sd_card_manager_init());
ESP_ERROR_CHECK(lvgl_sd_fs_register());
```

Then LVGL can use SD card files through the `S:` drive path:

```c
lv_obj_t *img = lv_image_create(lv_screen_active());
lv_image_set_src(img, "S:/images/logo.png");
```

The public API is intentionally small:

```c
esp_err_t lvgl_sd_fs_register(void);
bool lvgl_sd_fs_is_registered(void);
bool lvgl_sd_fs_is_ready(void);
```

`lvgl_sd_fs_is_registered()` only reports whether the LVGL driver was
registered. `lvgl_sd_fs_is_ready()` additionally requires the SD manager to
report a mounted card.

## Important Notes

- Call `lvgl_sd_fs_register()` only after `lv_init()` and after the SD card is
  mounted.
- LVGL removes the `S:` drive prefix before calling this component's callbacks.
  For example, `S:/a.png` arrives internally as `/a.png`.
- File operations use ESP-IDF VFS and stdio, so the SD card must remain mounted
  while LVGL is reading assets.
- Repeated calls to `lvgl_sd_fs_register()` return `ESP_OK` without registering
  a second driver.
- The component currently focuses on reading assets for LVGL. `open_cb` maps
  LVGL write mode to stdio mode, but there is no LVGL `write_cb`, directory
  listing callback, remove callback, or rename callback yet.
- Cache size is currently `0` to keep behavior simple and RAM usage low.

## Future Attention

- Add write, remove, rename, or directory callbacks only when LVGL actually
  needs them.
- Consider enabling a small LVGL FS cache after SD loading is stable and RAM
  usage is measured.
- Add examples for supported image formats once the project chooses PNG/JPG/BMP
  or binary LVGL image assets.
- Keep SD card mount/unmount ownership inside `sd_card_manager`; this component
  should stay as the LVGL adapter layer.
