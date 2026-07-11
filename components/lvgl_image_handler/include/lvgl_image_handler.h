#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include "lvgl.h"



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

lv_obj_t* lvgl_image_handler_get_image_obj(void);

esp_err_t lvgl_image_handler_apply_scale_and_align(
    lv_obj_t *image_obj,
    uint32_t percent,
    lv_align_t align,
    int32_t offset_x,
    int32_t offset_y);