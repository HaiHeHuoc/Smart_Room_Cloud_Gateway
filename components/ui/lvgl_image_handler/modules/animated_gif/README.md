# Animated GIF module

Private `lvgl_image_handler` submodule for the optional LVGL GIF decode/object
path.

- **Owner:** `lvgl_image_handler`
- **Visibility:** private header/source; callers use the public image-handler API.
- **Runtime gate:** source is compiled only when the existing LVGL GIF option is
  enabled.
- **Promotion rule:** keep private while GIF handling shares the parent image
  lifecycle and LVGL ownership contract.
