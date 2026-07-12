# lvgl_image_handler Component Notes

## Purpose

`lvgl_image_handler` loads JPG, PNG, and animated GIF assets from the LVGL SD
filesystem, scales each source to fit the active screen while preserving its
aspect ratio, converts the output to RGB565, and owns the active LVGL image
object and its frame buffers.

## What Is Done

- Supports explicit JPG, PNG, and GIF public APIs.
- Uses LVGL's configured image decoder path for JPG.
- Decodes PNG rows with `libpng` instead of allocating a full RGBA source image.
- Supports non-interlaced and Adam7-interlaced PNG files.
- Plays GIF frames from SD using an LVGL timer and a locally wrapped
  AnimatedGIF decoder.
- Handles GIF transparency and disposal methods.
- Uses front/back RGB565 GIF buffers so a frame is displayed only after its
  decode completes.
- Allocates the second GIF frame from ESP heap rather than LVGL's built-in
  memory pool.
- Replaces and releases the previous image when a new image is activated.
- Provides percentage scale and LVGL alignment for the active image object.

## Dependencies And Setup

The required initialization order is:

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(sd_card_manager_init());
ESP_ERROR_CHECK(lvgl_sd_fs_register());
ESP_ERROR_CHECK(ui_manager_lvgl_start_UI_task());
```

Image paths use the LVGL SD drive, for example `S:/images/photo.jpg`.
`espressif/libpng` is declared in `idf_component.yml`. GIF source is compiled
only when `CONFIG_LV_USE_GIF` is enabled.

## Public API

| API | Role |
| --- | --- |
| `lvgl_image_handler_show_jpg()` | Decode and show a JPG image. |
| `lvgl_image_handler_show_png()` | Stream-decode and show a PNG image. |
| `lvgl_image_handler_show_gif()` | Start a looping animated GIF. |
| `lvgl_image_handler_clear()` | Delete the active object and release image/GIF resources. |
| `lvgl_image_handler_has_active_object()` | Report whether an image object is active. |
| `lvgl_image_handler_example_task()` | Start the built-in PNG/JPG/GIF cycling demonstration. |

Typical usage:

```c
ui_manager_lvgl_wait_for_mutex();

esp_err_t ret = lvgl_image_handler_show_png("S:/Hinh.png");

ui_manager_lvgl_release_mutex();
```

## Important Notes

- These APIs create, modify, or delete LVGL objects and timers. Call them from
  the LVGL task or while holding the `ui_manager_lvgl` mutex.
- The SD manager must be mounted and `lvgl_sd_fs` must be registered; otherwise
  show calls return `ESP_ERR_INVALID_STATE`.
- Format is selected by the JPG/PNG/GIF API, not inferred from the filename
  extension.
- The initial decoded frame is already sized to fit the active screen.
- Format dispatch, active-object access, and example scale/alignment are
  internal implementation details rather than public component APIs.
- GIF playback loops internally. A caller that clears or replaces the image
  before the animation duration ends will naturally stop it.
- Large images still need enough memory for the fitted RGB565 output frame.
  GIF additionally needs a second output frame and decoder working memory.
- The GIF decoder rejects source canvases above 3840 x 2160 pixels and source
  widths above its configured `MAX_WIDTH` of 4096.

## Future Attention

- Add a deinit API only if LVGL/display shutdown becomes a real requirement.
- Consider an API that decodes directly to a requested output rectangle if
  repeated post-decode scaling becomes common.
- Measure SD throughput and GIF decode time on target hardware before raising
  animation resolution or frame rate.
- Add automated tests for PNG alpha/interlace and GIF disposal edge cases.
