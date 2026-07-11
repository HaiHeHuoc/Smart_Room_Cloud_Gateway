#pragma once

#include "esp_err.h"
#include <stdbool.h>



typedef enum
{
    LVGL_IMAGE_HANDLER_FORMAT_JPG = 0,
    LVGL_IMAGE_HANDLER_FORMAT_PNG,
    LVGL_IMAGE_HANDLER_FORMAT_GIF,
} lvgl_image_handler_format_t;

esp_err_t lvgl_image_handler_show(const char *path,
                                  lvgl_image_handler_format_t format);

esp_err_t lvgl_image_handler_show_jpg(const char *path);

esp_err_t lvgl_image_handler_show_png(const char *path);

esp_err_t lvgl_image_handler_show_gif(const char *path);

esp_err_t lvgl_image_handler_clear(void);

bool lvgl_image_handler_has_active_object(void);