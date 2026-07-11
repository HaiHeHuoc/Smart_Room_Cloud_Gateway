#include "lvgl_image_handler.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

#include <limits.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "png.h"

#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"
#if LV_USE_GIF
#include "lvgl_image_handler_animated_gif.h"
#endif
#include "lvgl_sd_fs.h"


/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */
#define PNG_RGBA_BYTES_PER_PIXEL 4U
#define GIF_MAX_SOURCE_PIXELS (3840ULL * 2160ULL)

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
static const char* TAG = "LVGL_IMAGE_HANDLER";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */
#if LV_USE_GIF
typedef struct {
    GIFIMAGE decoder;
    lv_obj_t *object;
    lv_draw_buf_t *draw_buf;
    lv_draw_buf_t *back_draw_buf;
    lv_draw_buf_t *decode_draw_buf;
    lv_draw_buf_t back_draw_buf_storage;
    uint8_t *back_draw_buf_data;
    lv_timer_t *timer;
    uint8_t *restore_buffer;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t scaled_width;
    uint32_t scaled_height;
    uint32_t output_offset_x;
    uint32_t output_offset_y;
    uint32_t frame_x;
    uint32_t frame_y;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t decoded_line_count;
    uint16_t background_color;
    uint8_t disposal_method;
    bool decoder_open;
    bool frame_started;
    bool restore_valid;
    bool restore_allocation_warning_logged;
} lvgl_image_handler_gif_state_t;
#endif

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static lv_obj_t *s_active_object = NULL;
static lv_draw_buf_t *s_active_draw_buf = NULL;
#if LV_USE_GIF
static lvgl_image_handler_gif_state_t *s_active_gif = NULL;
#endif


/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

#if LV_USE_GIF
static void lvgl_image_handler_release_gif_state(
    lvgl_image_handler_gif_state_t *state)
{
    if (state == NULL) {
        return;
    }

    if (state->timer != NULL) {
        lv_timer_delete(state->timer);
    }

    if (state->decoder_open) {
        GIF_close(&state->decoder);
    }

    free(state->restore_buffer);

    heap_caps_free(state->back_draw_buf_data);

    free(state);
}
#endif

static void lvgl_image_handler_clear_internal(void)
{
    if ((s_active_object == NULL) && (s_active_draw_buf == NULL)
#if LV_USE_GIF
        && (s_active_gif == NULL)
#endif
       ) {
        ESP_LOGI(TAG, "Available to display a new image");
        return;
    }

    ESP_LOGI(TAG, "Delete the current image");

#if LV_USE_GIF
    lvgl_image_handler_gif_state_t *old_gif = s_active_gif;
    s_active_gif = NULL;
    lvgl_image_handler_release_gif_state(old_gif);
#endif

    if (s_active_draw_buf != NULL) {
        lv_image_cache_drop(s_active_draw_buf);
    }

    if (s_active_object != NULL) {
        lv_obj_delete(s_active_object);
    }

    if (s_active_draw_buf != NULL) {
        lv_draw_buf_destroy(s_active_draw_buf);
    }

    s_active_object = NULL;
    s_active_draw_buf = NULL;
}

static void lvgl_image_handler_calculate_contain_size(
    uint32_t image_width,
    uint32_t image_height,
    uint32_t frame_width,
    uint32_t frame_height,
    uint32_t *output_width,
    uint32_t *output_height)
{
    if ((image_width == 0U)  ||
        (image_height == 0U) ||
        (frame_width == 0U)  ||
        (frame_height == 0U) ||
        (output_width == NULL) ||
        (output_height == NULL)) {
        return;
    }

    /*
     * Compare both aspect ratios without floating-point arithmetic. The
     * larger ratio determines which frame edge limits the output size.
     */
    if (((uint64_t)image_width * frame_height) >=
        ((uint64_t)image_height * frame_width)) {
        *output_width = frame_width;
        *output_height = (uint32_t)(((uint64_t)image_height * frame_width) /
                                    image_width);
    }
    else {
        *output_height = frame_height;
        *output_width = (uint32_t)(((uint64_t)image_width * frame_height) /
                                   image_height);
    }

    if (*output_width == 0U) {
        *output_width = 1U;
    }

    if (*output_height == 0U) {
        *output_height = 1U;
    }
}

static uint32_t lvgl_image_handler_divide_round_up(uint64_t numerator,
                                                   uint32_t denominator)
{
    return (uint32_t)((numerator + denominator - 1U) / denominator);
}

#if LV_USE_GIF
static void lvgl_image_handler_write_rgb565(uint8_t *row,
                                            uint32_t x,
                                            uint16_t color)
{
    row[x * 2U] = (uint8_t)(color & 0xFFU);
    row[(x * 2U) + 1U] = (uint8_t)(color >> 8);
}

static uint32_t lvgl_image_handler_map_scaled_pixel_to_source(
    uint32_t scaled_index,
    uint32_t source_size,
    uint32_t scaled_size)
{
    uint32_t source_index =
        (uint32_t)((((uint64_t)scaled_index * 2U + 1U) * source_size) /
                   ((uint64_t)scaled_size * 2U));

    if (source_index >= source_size) {
        source_index = source_size - 1U;
    }

    return source_index;
}

static void lvgl_image_handler_gif_fill_source_rect(
    lvgl_image_handler_gif_state_t *state,
    uint32_t source_x,
    uint32_t source_y,
    uint32_t source_rect_width,
    uint32_t source_rect_height,
    uint16_t color)
{
    if ((state == NULL) || (state->decode_draw_buf == NULL) ||
        (source_rect_width == 0U) || (source_rect_height == 0U)) {
        return;
    }

    const uint64_t requested_x_end =
        (uint64_t)source_x + source_rect_width;
    const uint64_t requested_y_end =
        (uint64_t)source_y + source_rect_height;
    const uint32_t source_x_end =
        requested_x_end < state->source_width
            ? (uint32_t)requested_x_end
            : state->source_width;
    const uint32_t source_y_end =
        requested_y_end < state->source_height
            ? (uint32_t)requested_y_end
            : state->source_height;

    for (uint32_t scaled_y = 0U;
         scaled_y < state->scaled_height;
         ++scaled_y) {
        const uint32_t mapped_y =
            lvgl_image_handler_map_scaled_pixel_to_source(
                scaled_y,
                state->source_height,
                state->scaled_height);
        if ((mapped_y < source_y) || (mapped_y >= source_y_end)) {
            continue;
        }

        uint8_t *output_row =
            state->decode_draw_buf->data +
            ((scaled_y + state->output_offset_y) *
             state->decode_draw_buf->header.stride);

        for (uint32_t scaled_x = 0U;
             scaled_x < state->scaled_width;
             ++scaled_x) {
            const uint32_t mapped_x =
                lvgl_image_handler_map_scaled_pixel_to_source(
                    scaled_x,
                    state->source_width,
                    state->scaled_width);
            if ((mapped_x >= source_x) && (mapped_x < source_x_end)) {
                lvgl_image_handler_write_rgb565(
                    output_row,
                    scaled_x + state->output_offset_x,
                    color);
            }
        }
    }
}

static void lvgl_image_handler_gif_apply_disposal(
    lvgl_image_handler_gif_state_t *state)
{
    if (state == NULL) {
        return;
    }

    if (state->disposal_method == 2U) {
        lvgl_image_handler_gif_fill_source_rect(state,
                                                state->frame_x,
                                                state->frame_y,
                                                state->frame_width,
                                                state->frame_height,
                                                state->background_color);
    }
    else if ((state->disposal_method == 3U) && state->restore_valid) {
        memcpy(state->decode_draw_buf->data,
               state->restore_buffer,
               state->decode_draw_buf->data_size);
    }

    state->disposal_method = 0U;
    state->restore_valid = false;
}

static void lvgl_image_handler_gif_draw_cb(GIFDRAW *draw)
{
    if ((draw == NULL) || (draw->pUser == NULL) ||
        (draw->pPixels == NULL) || (draw->pPalette == NULL)) {
        return;
    }

    lvgl_image_handler_gif_state_t *state =
        (lvgl_image_handler_gif_state_t *)draw->pUser;

    if (!state->frame_started) {
        state->frame_started = true;
        state->frame_x = (uint32_t)draw->iX;
        state->frame_y = (uint32_t)draw->iY;
        state->frame_width = (uint32_t)draw->iWidth;
        state->frame_height = (uint32_t)draw->iHeight;
        state->disposal_method = draw->ucDisposalMethod;
        state->background_color =
            state->decoder.pPalette[draw->ucBackground];

        if (state->disposal_method == 3U) {
            if (state->restore_buffer == NULL) {
                state->restore_buffer =
                    malloc(state->decode_draw_buf->data_size);
            }

            if (state->restore_buffer != NULL) {
                memcpy(state->restore_buffer,
                       state->decode_draw_buf->data,
                       state->decode_draw_buf->data_size);
                state->restore_valid = true;
            }
            else if (!state->restore_allocation_warning_logged) {
                ESP_LOGW(TAG,
                         "No memory for GIF restore-previous buffer; preserving the current frame instead");
                state->restore_allocation_warning_logged = true;
            }
        }
    }

    const uint32_t source_y = (uint32_t)(draw->iY + draw->y);
    if (source_y >= state->source_height) {
        return;
    }

    ++state->decoded_line_count;
    if ((state->decoded_line_count & 0x1FU) == 0U) {
        vTaskDelay(1);
    }

    const uint32_t source_x_start = (uint32_t)draw->iX;
    const uint64_t requested_x_end =
        (uint64_t)source_x_start + (uint32_t)draw->iWidth;
    const uint32_t source_x_end =
        requested_x_end < state->source_width
            ? (uint32_t)requested_x_end
            : state->source_width;

    for (uint32_t scaled_y = 0U;
         scaled_y < state->scaled_height;
         ++scaled_y) {
        const uint32_t mapped_y =
            lvgl_image_handler_map_scaled_pixel_to_source(
                scaled_y,
                state->source_height,
                state->scaled_height);
        if (mapped_y != source_y) {
            continue;
        }

        uint8_t *output_row =
            state->decode_draw_buf->data +
            ((scaled_y + state->output_offset_y) *
             state->decode_draw_buf->header.stride);

        for (uint32_t scaled_x = 0U;
             scaled_x < state->scaled_width;
             ++scaled_x) {
            const uint32_t mapped_x =
                lvgl_image_handler_map_scaled_pixel_to_source(
                    scaled_x,
                    state->source_width,
                    state->scaled_width);
            if ((mapped_x < source_x_start) ||
                (mapped_x >= source_x_end)) {
                continue;
            }

            const uint8_t palette_index =
                draw->pPixels[mapped_x - source_x_start];
            if (draw->ucHasTransparency &&
                (palette_index == draw->ucTransparent)) {
                continue;
            }

            lvgl_image_handler_write_rgb565(
                output_row,
                scaled_x + state->output_offset_x,
                draw->pPalette[palette_index]);
        }
    }
}

static esp_err_t lvgl_image_handler_gif_decode_next_frame(
    lvgl_image_handler_gif_state_t *state,
    uint32_t *next_delay_ms)
{
    ESP_RETURN_ON_FALSE(state != NULL && next_delay_ms != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid GIF frame state");

    const bool frame_is_active = (state->object != NULL);
    if (frame_is_active) {
        ESP_RETURN_ON_FALSE(state->back_draw_buf != NULL,
                            ESP_ERR_INVALID_STATE,
                            TAG,
                            "GIF back buffer is not available");

        memcpy(state->back_draw_buf->data,
               state->draw_buf->data,
               state->draw_buf->data_size);
        state->decode_draw_buf = state->back_draw_buf;
    }
    else {
        state->decode_draw_buf = state->draw_buf;
    }

    lvgl_image_handler_gif_apply_disposal(state);
    state->frame_started = false;
    state->decoded_line_count = 0U;
    state->decoder.iError = GIF_SUCCESS;

    int frame_delay_ms = 0;
    const int has_next = GIF_playFrame(&state->decoder,
                                       &frame_delay_ms,
                                       state);
    const int decoder_error = GIF_getLastError(&state->decoder);

    if (!state->frame_started &&
        (decoder_error != GIF_SUCCESS) &&
        (decoder_error != GIF_EMPTY_FRAME)) {
        ESP_LOGE(TAG,
                 "GIF frame decode failed: decoder error=%d",
                 decoder_error);
        state->decode_draw_buf = NULL;
        return ESP_FAIL;
    }

    if (frame_delay_ms <= 0) {
        frame_delay_ms = 100;
    }

    *next_delay_ms = (uint32_t)frame_delay_ms;

    if (frame_is_active) {
        memcpy(state->draw_buf->data,
               state->decode_draw_buf->data,
               state->draw_buf->data_size);
        lv_draw_buf_flush_cache(state->draw_buf, NULL);
        lv_image_cache_drop(state->draw_buf);
        lv_obj_invalidate(state->object);
    }
    else {
        lv_draw_buf_flush_cache(state->draw_buf, NULL);
    }

    state->decode_draw_buf = NULL;

    if (has_next <= 0) {
        GIF_reset(&state->decoder);
        ESP_LOGD(TAG,
                 "GIF animation reached its final frame; restart from frame zero on next tick");
    }

    return ESP_OK;
}

static void lvgl_image_handler_gif_timer_cb(lv_timer_t *timer)
{
    lvgl_image_handler_gif_state_t *state =
        (lvgl_image_handler_gif_state_t *)lv_timer_get_user_data(timer);
    uint32_t next_delay_ms = 100U;

    const esp_err_t ret =
        lvgl_image_handler_gif_decode_next_frame(state, &next_delay_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stop GIF animation after a decode error");
        lv_timer_pause(timer);
        return;
    }

    if (state->object != NULL) {
        lv_display_t *display = lv_obj_get_display(state->object);
        if (display != NULL) {
            /* Ensure this decoded frame reaches the LCD before advancing. */
            lv_refr_now(display);
        }
    }

    lv_timer_set_period(timer, next_delay_ms);
    lv_timer_reset(timer);
}
#endif

static void lvgl_image_handler_png_error_cb(png_structp png_ptr,
                                            png_const_charp message)
{
    ESP_LOGE(TAG,
             "libpng error: %s",
             message != NULL ? message : "unknown error");
    png_longjmp(png_ptr, 1);
}

static void lvgl_image_handler_png_warning_cb(png_structp png_ptr,
                                              png_const_charp message)
{
    (void)png_ptr;

    ESP_LOGW(TAG,
             "libpng warning: %s",
             message != NULL ? message : "unknown warning");
}

static void lvgl_image_handler_png_read_cb(png_structp png_ptr,
                                           png_bytep output,
                                           png_size_t byte_count)
{
    lv_fs_file_t *file = (lv_fs_file_t *)png_get_io_ptr(png_ptr);

    if ((file == NULL) || (output == NULL) ||
        (byte_count > UINT32_MAX)) {
        png_error(png_ptr, "Invalid PNG read request");
        return;
    }

    uint32_t bytes_read = 0U;
    const lv_fs_res_t fs_result =
        lv_fs_read(file, output, (uint32_t)byte_count, &bytes_read);

    if ((fs_result != LV_FS_RES_OK) ||
        (bytes_read != (uint32_t)byte_count)) {
        png_error(png_ptr, "Failed to read PNG data from LVGL filesystem");
    }
}

static void lvgl_image_handler_write_png_pixel(
    uint8_t *output_row,
    uint32_t output_x,
    const uint8_t *source_pixel)
{
    const uint32_t alpha = source_pixel[3];

    /* libpng supplies R, G, B, A. Composite alpha onto black. */
    const uint8_t red =
        (uint8_t)(((uint32_t)source_pixel[0] * alpha + 127U) / 255U);
    const uint8_t green =
        (uint8_t)(((uint32_t)source_pixel[1] * alpha + 127U) / 255U);
    const uint8_t blue =
        (uint8_t)(((uint32_t)source_pixel[2] * alpha + 127U) / 255U);
    const uint16_t rgb565 =
        (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                   ((uint16_t)(green & 0xFCU) << 3) |
                   ((uint16_t)blue >> 3));

    output_row[output_x * 2U] = (uint8_t)(rgb565 & 0xFFU);
    output_row[(output_x * 2U) + 1U] = (uint8_t)(rgb565 >> 8);
}

static void lvgl_image_handler_write_png_row(
    const uint8_t *source_row,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t source_y,
    uint32_t scaled_width,
    uint32_t scaled_height,
    uint32_t output_offset_x,
    uint32_t output_offset_y,
    uint32_t *next_scaled_y,
    lv_draw_buf_t *output_draw_buf)
{
    while ((*next_scaled_y < scaled_height) &&
           (((uint64_t)(*next_scaled_y) * source_height) /
            scaled_height == source_y)) {
        uint8_t *output_row =
            output_draw_buf->data +
            ((*next_scaled_y + output_offset_y) *
             output_draw_buf->header.stride);

        for (uint32_t scaled_x = 0U;
             scaled_x < scaled_width;
             ++scaled_x) {
            const uint32_t source_x =
                (uint32_t)(((uint64_t)scaled_x * source_width) /
                           scaled_width);
            const uint8_t *source_pixel =
                source_row + (source_x * PNG_RGBA_BYTES_PER_PIXEL);
            const uint32_t output_x = scaled_x + output_offset_x;

            lvgl_image_handler_write_png_pixel(output_row,
                                                output_x,
                                                source_pixel);
        }

        ++(*next_scaled_y);
    }
}

static void lvgl_image_handler_write_interlaced_png_row(
    const uint8_t *source_row,
    uint32_t pass,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t source_y,
    uint32_t scaled_width,
    uint32_t scaled_height,
    uint32_t output_offset_x,
    uint32_t output_offset_y,
    lv_draw_buf_t *output_draw_buf)
{
    for (uint32_t scaled_y = 0U;
         scaled_y < scaled_height;
         ++scaled_y) {
        const uint32_t mapped_source_y =
            (uint32_t)(((uint64_t)scaled_y * source_height) /
                       scaled_height);
        if (mapped_source_y != source_y) {
            continue;
        }

        uint8_t *output_row =
            output_draw_buf->data +
            ((scaled_y + output_offset_y) *
             output_draw_buf->header.stride);

        for (uint32_t scaled_x = 0U;
             scaled_x < scaled_width;
             ++scaled_x) {
            const uint32_t source_x =
                (uint32_t)(((uint64_t)scaled_x * source_width) /
                           scaled_width);
            if (!PNG_COL_IN_INTERLACE_PASS(source_x, pass)) {
                continue;
            }

            const uint32_t pass_x =
                (source_x - PNG_PASS_START_COL(pass)) >>
                PNG_PASS_COL_SHIFT(pass);
            const uint8_t *source_pixel =
                source_row + (pass_x * PNG_RGBA_BYTES_PER_PIXEL);

            lvgl_image_handler_write_png_pixel(output_row,
                                                scaled_x + output_offset_x,
                                                source_pixel);
        }
    }
}

static esp_err_t lvgl_image_handler_decode_png_stream_to_frame(
    const char *path,
    uint32_t frame_width,
    uint32_t frame_height,
    lv_draw_buf_t **output_draw_buf)
{
    ESP_RETURN_ON_FALSE(path != NULL && output_draw_buf != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid streaming PNG arguments");

    *output_draw_buf = NULL;

    lv_fs_file_t file = {0};
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    lv_draw_buf_t *frame_draw_buf = NULL;
    uint8_t *source_row = NULL;
    esp_err_t ret = ESP_FAIL;
    bool file_is_open = false;

    if (lv_fs_open(&file, path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        ESP_LOGE(TAG, "Failed to open PNG file: %s", path);
        return ESP_FAIL;
    }
    file_is_open = true;

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                     NULL,
                                     lvgl_image_handler_png_error_cb,
                                     lvgl_image_handler_png_warning_cb);
    if (png_ptr == NULL) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create libpng read context");
        goto cleanup;
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create libpng info context");
        goto cleanup;
    }

    if (setjmp(png_jmpbuf(png_ptr)) != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    png_set_read_fn(png_ptr, &file, lvgl_image_handler_png_read_cb);
    png_read_info(png_ptr, info_ptr);

    const png_uint_32 source_width =
        png_get_image_width(png_ptr, info_ptr);
    const png_uint_32 source_height =
        png_get_image_height(png_ptr, info_ptr);
    const int source_bit_depth =
        png_get_bit_depth(png_ptr, info_ptr);
    const int source_color_type =
        png_get_color_type(png_ptr, info_ptr);
    const int interlace_type =
        png_get_interlace_type(png_ptr, info_ptr);

    if ((source_width == 0U) || (source_height == 0U)) {
        ESP_LOGE(TAG, "PNG has invalid dimensions");
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (source_bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }

    if (source_color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }

    if ((source_color_type == PNG_COLOR_TYPE_GRAY) &&
        (source_bit_depth < 8)) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }

    const bool has_transparency =
        png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) != 0U;
    if (has_transparency) {
        png_set_tRNS_to_alpha(png_ptr);
    }

    if ((source_color_type == PNG_COLOR_TYPE_GRAY) ||
        (source_color_type == PNG_COLOR_TYPE_GRAY_ALPHA)) {
        png_set_gray_to_rgb(png_ptr);
    }

    if (((source_color_type & PNG_COLOR_MASK_ALPHA) == 0) &&
        !has_transparency) {
        png_set_add_alpha(png_ptr, 0xFFU, PNG_FILLER_AFTER);
    }

    png_read_update_info(png_ptr, info_ptr);

    if (png_get_channels(png_ptr, info_ptr) !=
        PNG_RGBA_BYTES_PER_PIXEL) {
        ESP_LOGE(TAG, "libpng did not produce RGBA8888 rows");
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    const png_size_t source_row_size =
        (png_size_t)source_width * PNG_RGBA_BYTES_PER_PIXEL;

    uint32_t scaled_width = 0U;
    uint32_t scaled_height = 0U;
    lvgl_image_handler_calculate_contain_size(source_width,
                                              source_height,
                                              frame_width,
                                              frame_height,
                                              &scaled_width,
                                              &scaled_height);

    frame_draw_buf = lv_draw_buf_create(frame_width,
                                        frame_height,
                                        LV_COLOR_FORMAT_RGB565,
                                        LV_STRIDE_AUTO);
    if (frame_draw_buf == NULL) {
        ESP_LOGE(TAG, "No memory for RGB565 PNG frame");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    lv_draw_buf_clear(frame_draw_buf, NULL);

    source_row = malloc(source_row_size);
    if (source_row == NULL) {
        ESP_LOGE(TAG,
                 "No memory for %u-byte streaming PNG row",
                 (unsigned int)source_row_size);
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* Resources above are stable if libpng jumps out during row decoding. */
    if (setjmp(png_jmpbuf(png_ptr)) != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    const uint32_t output_offset_x =
        (frame_width - scaled_width) / 2U;
    const uint32_t output_offset_y =
        (frame_height - scaled_height) / 2U;
    uint32_t next_scaled_y = 0U;

    if (interlace_type == PNG_INTERLACE_NONE) {
        for (uint32_t source_y = 0U;
             source_y < source_height;
             ++source_y) {
            png_read_row(png_ptr, source_row, NULL);
            lvgl_image_handler_write_png_row(source_row,
                                             source_width,
                                             source_height,
                                             source_y,
                                             scaled_width,
                                             scaled_height,
                                             output_offset_x,
                                             output_offset_y,
                                             &next_scaled_y,
                                             frame_draw_buf);

            if ((source_y & 0x1FU) == 0x1FU) {
                vTaskDelay(1);
            }
        }
    }
    else {
        for (uint32_t pass = 0U;
             pass < PNG_INTERLACE_ADAM7_PASSES;
             ++pass) {
            const uint32_t pass_width =
                PNG_PASS_COLS(source_width, pass);
            const uint32_t pass_height =
                PNG_PASS_ROWS(source_height, pass);

            if ((pass_width == 0U) || (pass_height == 0U)) {
                continue;
            }

            for (uint32_t pass_y = 0U;
                 pass_y < pass_height;
                 ++pass_y) {
                png_read_row(png_ptr, source_row, NULL);

                const uint32_t source_y =
                    PNG_ROW_FROM_PASS_ROW(pass_y, pass);
                lvgl_image_handler_write_interlaced_png_row(
                    source_row,
                    pass,
                    source_width,
                    source_height,
                    source_y,
                    scaled_width,
                    scaled_height,
                    output_offset_x,
                    output_offset_y,
                    frame_draw_buf);

                if ((pass_y & 0x1FU) == 0x1FU) {
                    vTaskDelay(1);
                }
            }
        }

        next_scaled_y = scaled_height;
    }

    png_read_end(png_ptr, NULL);

    if (next_scaled_y != scaled_height) {
        ESP_LOGE(TAG, "Streaming PNG scaler did not produce every output row");
        ret = ESP_FAIL;
        goto cleanup;
    }

    lv_draw_buf_flush_cache(frame_draw_buf, NULL);
    *output_draw_buf = frame_draw_buf;
    frame_draw_buf = NULL;
    ret = ESP_OK;

    ESP_LOGI(TAG,
             "PNG stream: path=%s, source=%lux%lu, frame=%lux%lu, scaled=%lux%lu, row=%u bytes",
             path,
             (unsigned long)source_width,
             (unsigned long)source_height,
             (unsigned long)frame_width,
             (unsigned long)frame_height,
             (unsigned long)scaled_width,
             (unsigned long)scaled_height,
             (unsigned int)source_row_size);

cleanup:
    free(source_row);

    if ((png_ptr != NULL) || (info_ptr != NULL)) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    }

    if (file_is_open) {
        lv_fs_close(&file);
    }

    if (frame_draw_buf != NULL) {
        lv_draw_buf_destroy(frame_draw_buf);
    }

    return ret;
}

static esp_err_t lvgl_image_handler_copy_decoded_area(
    const lv_image_decoder_dsc_t *decoder_dsc,
    const lv_area_t *decoded_area,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t scaled_width,
    uint32_t scaled_height,
    uint32_t output_offset_x,
    uint32_t output_offset_y,
    lv_draw_buf_t *output_draw_buf)
{
    ESP_RETURN_ON_FALSE(decoder_dsc != NULL && decoded_area != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid image decoder data");

    const lv_draw_buf_t *decoded = decoder_dsc->decoded;

    ESP_RETURN_ON_FALSE(decoded != NULL && decoded->data != NULL,
                        ESP_FAIL,
                        TAG,
                        "Image decoder returned no pixel data");

    uint32_t source_bytes_per_pixel = 0U;

    switch (decoded->header.cf) {
    case LV_COLOR_FORMAT_RGB888:
        source_bytes_per_pixel = 3U;
        break;

    case LV_COLOR_FORMAT_ARGB8888:
        source_bytes_per_pixel = 4U;
        break;

    default:
        ESP_LOGE(TAG,
                 "Unsupported decoded image color format: %u",
                 (unsigned int)decoded->header.cf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_FALSE(decoded_area->x1 >= 0 && decoded_area->y1 >= 0 &&
                        decoded_area->x2 >= decoded_area->x1 &&
                        decoded_area->y2 >= decoded_area->y1,
                        ESP_FAIL,
                        TAG,
                        "Invalid decoded image area");

    const uint32_t area_width = (uint32_t)lv_area_get_width(decoded_area);
    const uint32_t area_height = (uint32_t)lv_area_get_height(decoded_area);

    ESP_RETURN_ON_FALSE(decoded->header.w == area_width &&
                        decoded->header.h == area_height &&
                        decoded->header.stride >=
                            (area_width * source_bytes_per_pixel),
                        ESP_FAIL,
                        TAG,
                        "Decoded image area and pixel buffer do not match");

    const bool source_is_premultiplied =
        lv_draw_buf_has_flag(decoded, LV_IMAGE_FLAGS_PREMULTIPLIED);

    const uint32_t scaled_x_start = lvgl_image_handler_divide_round_up(
        (uint64_t)(uint32_t)decoded_area->x1 * scaled_width,
        source_width);
    uint32_t scaled_x_end = lvgl_image_handler_divide_round_up(
        (uint64_t)((uint32_t)decoded_area->x2 + 1U) * scaled_width,
        source_width);

    const uint32_t scaled_y_start = lvgl_image_handler_divide_round_up(
        (uint64_t)(uint32_t)decoded_area->y1 * scaled_height,
        source_height);
    uint32_t scaled_y_end = lvgl_image_handler_divide_round_up(
        (uint64_t)((uint32_t)decoded_area->y2 + 1U) * scaled_height,
        source_height);

    if (scaled_x_end > scaled_width) {
        scaled_x_end = scaled_width;
    }

    if (scaled_y_end > scaled_height) {
        scaled_y_end = scaled_height;
    }

    for (uint32_t scaled_y = scaled_y_start;
         scaled_y < scaled_y_end;
         ++scaled_y) {
        const uint32_t source_y =
            (uint32_t)(((uint64_t)scaled_y * source_height) /
                       scaled_height);
        const uint32_t source_row =
            source_y - (uint32_t)decoded_area->y1;

        const uint8_t *source_data =
            decoded->data + (source_row * decoded->header.stride);
        uint8_t *output_data =
            output_draw_buf->data +
            ((scaled_y + output_offset_y) * output_draw_buf->header.stride);

        for (uint32_t scaled_x = scaled_x_start;
             scaled_x < scaled_x_end;
             ++scaled_x) {
            const uint32_t source_x =
                (uint32_t)(((uint64_t)scaled_x * source_width) /
                           scaled_width);
            const uint32_t source_column =
                source_x - (uint32_t)decoded_area->x1;
            const uint8_t *source_pixel =
                source_data +
                (source_column * source_bytes_per_pixel);

            /* LVGL stores RGB888/ARGB8888 channels in B, G, R, [A] order. */
            uint8_t blue = source_pixel[0];
            uint8_t green = source_pixel[1];
            uint8_t red = source_pixel[2];

            if ((source_bytes_per_pixel == 4U) &&
                !source_is_premultiplied) {
                const uint32_t alpha = source_pixel[3];

                /* Composite transparent PNG pixels onto the black frame. */
                blue = (uint8_t)(((uint32_t)blue * alpha + 127U) / 255U);
                green = (uint8_t)(((uint32_t)green * alpha + 127U) / 255U);
                red = (uint8_t)(((uint32_t)red * alpha + 127U) / 255U);
            }

            const uint16_t rgb565 =
                (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                           ((uint16_t)(green & 0xFCU) << 3) |
                           ((uint16_t)blue >> 3));
            const uint32_t output_column = scaled_x + output_offset_x;

            output_data[(output_column * 2U)] = (uint8_t)(rgb565 & 0xFFU);
            output_data[(output_column * 2U) + 1U] =
                (uint8_t)(rgb565 >> 8);
        }
    }

    return ESP_OK;
}

static esp_err_t lvgl_image_handler_decode_image_to_frame(
    const char *path,
    const char *format_name,
    uint32_t source_width,
    uint32_t source_height,
    uint32_t frame_width,
    uint32_t frame_height,
    uint32_t scaled_width,
    uint32_t scaled_height,
    lv_draw_buf_t **output_draw_buf)
{
    ESP_RETURN_ON_FALSE(output_draw_buf != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Output draw buffer pointer is NULL");

    *output_draw_buf = NULL;

    lv_image_decoder_dsc_t decoder_dsc = {0};
    lv_draw_buf_t *frame_draw_buf = NULL;
    esp_err_t ret = ESP_FAIL;

    const lv_image_decoder_args_t decoder_args = {
        .no_cache = true,
    };

    if (lv_image_decoder_open(&decoder_dsc,
                              path,
                              &decoder_args) != LV_RESULT_OK) {
        ESP_LOGE(TAG,
                 "Failed to open %s decoder for: %s",
                 format_name,
                 path);
        return ESP_FAIL;
    }

    if ((decoder_dsc.header.w != source_width) ||
        (decoder_dsc.header.h != source_height)) {
        ESP_LOGE(TAG,
                 "%s size changed while opening decoder: expected=%lux%lu, actual=%ux%u",
                 format_name,
                 (unsigned long)source_width,
                 (unsigned long)source_height,
                 (unsigned int)decoder_dsc.header.w,
                 (unsigned int)decoder_dsc.header.h);
        goto cleanup;
    }

    frame_draw_buf = lv_draw_buf_create(frame_width,
                                        frame_height,
                                        LV_COLOR_FORMAT_RGB565,
                                        LV_STRIDE_AUTO);
    if (frame_draw_buf == NULL) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,
                 "No memory for %lux%lu RGB565 image frame",
                 (unsigned long)frame_width,
                 (unsigned long)frame_height);
        goto cleanup;
    }

    /* Clear the unused contain area so aspect-ratio bars are black. */
    lv_draw_buf_clear(frame_draw_buf, NULL);

    const uint32_t output_offset_x = (frame_width - scaled_width) / 2U;
    const uint32_t output_offset_y = (frame_height - scaled_height) / 2U;
    const lv_area_t full_source_area = {
        .x1 = 0,
        .y1 = 0,
        .x2 = (int32_t)source_width - 1,
        .y2 = (int32_t)source_height - 1,
    };

    bool decode_complete = false;

    if (decoder_dsc.decoded != NULL) {
        ret = lvgl_image_handler_copy_decoded_area(&decoder_dsc,
                                                   &full_source_area,
                                                   source_width,
                                                   source_height,
                                                   scaled_width,
                                                   scaled_height,
                                                   output_offset_x,
                                                   output_offset_y,
                                                   frame_draw_buf);
        decode_complete = (ret == ESP_OK);
    }
    else {
        lv_area_t decoded_area = {
            .x1 = LV_COORD_MIN,
            .y1 = LV_COORD_MIN,
            .x2 = LV_COORD_MIN,
            .y2 = LV_COORD_MIN,
        };

        while (lv_image_decoder_get_area(&decoder_dsc,
                                         &full_source_area,
                                         &decoded_area) == LV_RESULT_OK) {
            ret = lvgl_image_handler_copy_decoded_area(&decoder_dsc,
                                                       &decoded_area,
                                                       source_width,
                                                       source_height,
                                                       scaled_width,
                                                       scaled_height,
                                                       output_offset_x,
                                                       output_offset_y,
                                                       frame_draw_buf);
            if (ret != ESP_OK) {
                break;
            }

            if (((uint32_t)decoded_area.x2 == (source_width - 1U)) &&
                ((uint32_t)decoded_area.y2 == (source_height - 1U))) {
                decode_complete = true;
                break;
            }
        }
    }

    if (!decode_complete) {
        ESP_LOGE(TAG,
                 "%s decoding ended before the complete image was read",
                 format_name);
        ret = ESP_FAIL;
        goto cleanup;
    }

    lv_draw_buf_flush_cache(frame_draw_buf, NULL);
    *output_draw_buf = frame_draw_buf;
    frame_draw_buf = NULL;
    ret = ESP_OK;

cleanup:
    lv_image_decoder_close(&decoder_dsc);

    if (frame_draw_buf != NULL) {
        lv_draw_buf_destroy(frame_draw_buf);
    }

    return ret;
}

static esp_err_t lvgl_image_handler_activate_frame(
    lv_obj_t *screen,
    lv_draw_buf_t *new_draw_buf,
    uint32_t frame_width,
    uint32_t frame_height,
    const char *format_name)
{
    ESP_RETURN_ON_FALSE(screen != NULL && new_draw_buf != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid image frame");

    lv_obj_t *new_image_obj = lv_image_create(screen);

    if (new_image_obj == NULL) {
        lv_draw_buf_destroy(new_draw_buf);
        ESP_LOGE(TAG, "Failed to create LVGL image object");
        return ESP_ERR_NO_MEM;
    }

    lv_image_set_src(new_image_obj, new_draw_buf);
    lv_obj_set_size(new_image_obj,
                    (int32_t)frame_width,
                    (int32_t)frame_height);
    lv_obj_center(new_image_obj);

    lv_obj_t *old_image_obj = s_active_object;
    lv_draw_buf_t *old_draw_buf = s_active_draw_buf;
#if LV_USE_GIF
    lvgl_image_handler_gif_state_t *old_gif = s_active_gif;
    s_active_gif = NULL;
#endif
    s_active_object = new_image_obj;
    s_active_draw_buf = new_draw_buf;

#if LV_USE_GIF
    lvgl_image_handler_release_gif_state(old_gif);
#endif

    if (old_draw_buf != NULL) {
        lv_image_cache_drop(old_draw_buf);
    }

    if (old_image_obj != NULL) {
        lv_obj_delete(old_image_obj);
    }

    if (old_draw_buf != NULL) {
        lv_draw_buf_destroy(old_draw_buf);
    }

    ESP_LOGI(TAG,
             "%s image displayed from %lu-byte RGB565 frame",
             format_name,
             (unsigned long)new_draw_buf->data_size);

    return ESP_OK;
}

static esp_err_t lvgl_image_handler_show_static_image_internal(
    const char *path,
    const char *format_name)
{
    ESP_RETURN_ON_FALSE(
        path != NULL && path[0] != '\0',
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid %s path",
        format_name
    );

    /*
     * 1. Validate LVGL display and active screen.
     */
    lv_display_t *display = lv_display_get_default();

    ESP_RETURN_ON_FALSE(
        display != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "No default LVGL display"
    );

    lv_obj_t *screen = lv_display_get_screen_active(display);

    ESP_RETURN_ON_FALSE(
        screen != NULL,
        ESP_ERR_INVALID_STATE,
        TAG,
        "No active LVGL screen"
    );

    /*
     * Ensure the screen layout and dimensions are updated.
     */
    lv_obj_update_layout(screen);

    /*
     * 2. Read the image header through the registered decoder.
     */
    lv_image_header_t image_header = {0};

    const lv_result_t result =
        lv_image_decoder_get_info(path, &image_header);

    ESP_RETURN_ON_FALSE(
        result == LV_RESULT_OK,
        ESP_FAIL,
        TAG,
        "No LVGL decoder can read %s file: %s",
        format_name,
        path
    );

    const uint32_t frame_width =
        (uint32_t)lv_obj_get_content_width(screen);

    const uint32_t frame_height =
        (uint32_t)lv_obj_get_content_height(screen);

    ESP_RETURN_ON_FALSE(frame_width > 0U && frame_height > 0U,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "Active screen has no drawable area");

    uint32_t scaled_width = 0U;
    uint32_t scaled_height = 0U;

    lvgl_image_handler_calculate_contain_size(image_header.w,
                                              image_header.h,
                                              frame_width,
                                              frame_height,
                                              &scaled_width,
                                              &scaled_height);

    ESP_LOGI(
        TAG,
        "%s: path=%s, source=%lux%lu, frame=%lux%lu, scaled=%lux%lu",
        format_name,
        path,
        (unsigned long)image_header.w,
        (unsigned long)image_header.h,
        (unsigned long)frame_width,
        (unsigned long)frame_height,
        (unsigned long)scaled_width,
        (unsigned long)scaled_height
    );

    /*
     * Decode and resize the source into one screen-sized RGB565 buffer first,
     * then display that buffer without another LVGL transform.
     */
    lv_draw_buf_t *new_draw_buf = NULL;

    ESP_RETURN_ON_ERROR(
        lvgl_image_handler_decode_image_to_frame(path,
                                                 format_name,
                                                 image_header.w,
                                                 image_header.h,
                                                 frame_width,
                                                 frame_height,
                                                 scaled_width,
                                                 scaled_height,
                                                 &new_draw_buf),
        TAG,
        "Failed to decode and scale %s",
        format_name
    );

    return lvgl_image_handler_activate_frame(screen,
                                             new_draw_buf,
                                             frame_width,
                                             frame_height,
                                             format_name);
}

static esp_err_t lvgl_image_handler_show_jpg_internal(const char *path)
{
#if LV_USE_TJPGD

    return lvgl_image_handler_show_static_image_internal(path, "JPG");

#else

    ESP_LOGE(
        TAG,
        "JPG support is disabled because LV_USE_TJPGD is not enabled"
    );

    return ESP_ERR_NOT_SUPPORTED;

#endif
}

static esp_err_t lvgl_image_handler_show_png_internal(const char *path)
{
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid PNG path");

    lv_display_t *display = lv_display_get_default();
    ESP_RETURN_ON_FALSE(display != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "No default LVGL display");

    lv_obj_t *screen = lv_display_get_screen_active(display);
    ESP_RETURN_ON_FALSE(screen != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "No active LVGL screen");

    lv_obj_update_layout(screen);

    const int32_t frame_width_px = lv_obj_get_content_width(screen);
    const int32_t frame_height_px = lv_obj_get_content_height(screen);
    ESP_RETURN_ON_FALSE(frame_width_px > 0 && frame_height_px > 0,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "Active screen has no drawable area");

    const uint32_t frame_width = (uint32_t)frame_width_px;
    const uint32_t frame_height = (uint32_t)frame_height_px;
    lv_draw_buf_t *new_draw_buf = NULL;

    ESP_RETURN_ON_ERROR(
        lvgl_image_handler_decode_png_stream_to_frame(path,
                                                      frame_width,
                                                      frame_height,
                                                      &new_draw_buf),
        TAG,
        "Failed to stream and scale PNG"
    );

    return lvgl_image_handler_activate_frame(screen,
                                             new_draw_buf,
                                             frame_width,
                                             frame_height,
                                             "PNG");
}

static esp_err_t lvgl_image_handler_show_gif_internal(const char *path)
{
#if LV_USE_GIF
    ESP_RETURN_ON_FALSE(path != NULL && path[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Invalid GIF path");

    lv_display_t *display = lv_display_get_default();
    ESP_RETURN_ON_FALSE(display != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "No default LVGL display");

    lv_obj_t *screen = lv_display_get_screen_active(display);
    ESP_RETURN_ON_FALSE(screen != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "No active LVGL screen");

    lv_obj_update_layout(screen);

    const int32_t output_width_px = lv_obj_get_content_width(screen);
    const int32_t output_height_px = lv_obj_get_content_height(screen);
    ESP_RETURN_ON_FALSE(output_width_px > 0 && output_height_px > 0,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "Active screen has no drawable area");

    const uint32_t output_width = (uint32_t)output_width_px;
    const uint32_t output_height = (uint32_t)output_height_px;
    lvgl_image_handler_gif_state_t *state = calloc(1U, sizeof(*state));
    if (state == NULL) {
        ESP_LOGE(TAG, "No memory for GIF decoder state");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_FAIL;
    GIF_begin(&state->decoder, GIF_PALETTE_RGB565_LE);

    if (!GIF_openFile(&state->decoder,
                      path,
                      lvgl_image_handler_gif_draw_cb)) {
        const int decoder_error = GIF_getLastError(&state->decoder);

        if (state->decoder.GIFFile.fHandle.drv != NULL) {
            GIF_close(&state->decoder);
        }

        ESP_LOGE(TAG,
                 "Failed to open GIF: path=%s, decoder error=%d",
                 path,
                 decoder_error);

        if (decoder_error == GIF_TOO_WIDE) {
            ESP_LOGE(TAG,
                     "GIF source width exceeds the %u-pixel scanline limit",
                     (unsigned int)MAX_WIDTH);
            ret = ESP_ERR_NOT_SUPPORTED;
        }
        else if (decoder_error == GIF_ERROR_MEMORY) {
            ret = ESP_ERR_NO_MEM;
        }
        else {
            ret = ESP_FAIL;
        }
        goto cleanup;
    }
    state->decoder_open = true;

    state->source_width = (uint32_t)GIF_getCanvasWidth(&state->decoder);
    state->source_height = (uint32_t)GIF_getCanvasHeight(&state->decoder);
    if ((state->source_width == 0U) || (state->source_height == 0U)) {
        ESP_LOGE(TAG, "GIF has invalid dimensions");
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    if (((uint64_t)state->source_width * state->source_height) >
        GIF_MAX_SOURCE_PIXELS) {
        ESP_LOGE(TAG,
                 "GIF source is too large to decode safely: %lux%lu (maximum %llu pixels)",
                 (unsigned long)state->source_width,
                 (unsigned long)state->source_height,
                 GIF_MAX_SOURCE_PIXELS);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    lvgl_image_handler_calculate_contain_size(state->source_width,
                                              state->source_height,
                                              output_width,
                                              output_height,
                                              &state->scaled_width,
                                              &state->scaled_height);
    state->output_offset_x =
        (output_width - state->scaled_width) / 2U;
    state->output_offset_y =
        (output_height - state->scaled_height) / 2U;

    state->draw_buf = lv_draw_buf_create(output_width,
                                         output_height,
                                         LV_COLOR_FORMAT_RGB565,
                                         LV_STRIDE_AUTO);
    if (state->draw_buf == NULL) {
        ESP_LOGE(TAG, "No memory for RGB565 GIF frame");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    lv_draw_buf_clear(state->draw_buf, NULL);

    const uint32_t back_buffer_stride =
        lv_draw_buf_width_to_stride(output_width,
                                    LV_COLOR_FORMAT_RGB565);
    const uint32_t back_buffer_size =
        back_buffer_stride * output_height;

    state->back_draw_buf_data =
        heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN,
                                back_buffer_size,
                                MALLOC_CAP_8BIT);
    if (state->back_draw_buf_data == NULL) {
        ESP_LOGE(TAG,
                 "No ESP heap for %lu-byte GIF back buffer; largest block=%lu bytes",
                 (unsigned long)back_buffer_size,
                 (unsigned long)heap_caps_get_largest_free_block(
                     MALLOC_CAP_8BIT));
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    if (lv_draw_buf_init(&state->back_draw_buf_storage,
                         output_width,
                         output_height,
                         LV_COLOR_FORMAT_RGB565,
                         back_buffer_stride,
                         state->back_draw_buf_data,
                         back_buffer_size) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to initialize GIF back draw buffer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    state->back_draw_buf_storage.header.flags |=
        LV_IMAGE_FLAGS_MODIFIABLE;
    state->back_draw_buf = &state->back_draw_buf_storage;
    lv_draw_buf_clear(state->back_draw_buf, NULL);

    uint32_t next_delay_ms = 100U;
    ret = lvgl_image_handler_gif_decode_next_frame(state,
                                                   &next_delay_ms);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    state->object = lv_image_create(screen);
    if (state->object == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL GIF image object");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    lv_image_set_src(state->object, state->draw_buf);
    lv_obj_set_size(state->object,
                    output_width_px,
                    output_height_px);
    lv_obj_center(state->object);

    state->timer = lv_timer_create(lvgl_image_handler_gif_timer_cb,
                                   next_delay_ms,
                                   state);
    if (state->timer == NULL) {
        ESP_LOGE(TAG, "Failed to create GIF frame timer");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    lv_obj_t *old_image_obj = s_active_object;
    lv_draw_buf_t *old_draw_buf = s_active_draw_buf;
    lvgl_image_handler_gif_state_t *old_gif = s_active_gif;

    s_active_object = state->object;
    s_active_draw_buf = state->draw_buf;
    s_active_gif = state;

    lvgl_image_handler_release_gif_state(old_gif);

    if (old_draw_buf != NULL) {
        lv_image_cache_drop(old_draw_buf);
    }
    if (old_image_obj != NULL) {
        lv_obj_delete(old_image_obj);
    }
    if (old_draw_buf != NULL) {
        lv_draw_buf_destroy(old_draw_buf);
    }

    lv_obj_invalidate(state->object);
    lv_refr_now(display);
    lv_timer_reset(state->timer);

    ESP_LOGI(TAG,
             "GIF started: path=%s, source=%lux%lu, frame=%lux%lu, scaled=%lux%lu, mode=double-buffer, pixel buffers=%lu bytes",
             path,
             (unsigned long)state->source_width,
             (unsigned long)state->source_height,
             (unsigned long)output_width,
             (unsigned long)output_height,
             (unsigned long)state->scaled_width,
             (unsigned long)state->scaled_height,
             (unsigned long)(state->draw_buf->data_size * 2U));

    return ESP_OK;

cleanup:
    if (state->object != NULL) {
        lv_obj_delete(state->object);
    }
    if (state->draw_buf != NULL) {
        lv_draw_buf_destroy(state->draw_buf);
    }
    lvgl_image_handler_release_gif_state(state);
    return ret;

#else
    ESP_LOGE(TAG,
             "GIF support is disabled because LV_USE_GIF is not enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* Functions -------------------------------------------------------------- */
/* Implement non-static functions here. */
esp_err_t lvgl_image_handler_show(const char *path,
                                  lvgl_image_handler_format_t format)
{
    ESP_RETURN_ON_FALSE(path != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Image path is NULL");

    ESP_RETURN_ON_FALSE(path[0] != '\0',
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Image path is empty");

    ESP_RETURN_ON_FALSE(lvgl_sd_fs_is_ready(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "LVGL SD filesystem is not ready");

    switch (format) {
    case LVGL_IMAGE_HANDLER_FORMAT_JPG:
        ESP_LOGI(TAG, "Show JPG image: %s", path);
        return lvgl_image_handler_show_jpg_internal(path);

    case LVGL_IMAGE_HANDLER_FORMAT_PNG:
        ESP_LOGI(TAG, "Show PNG image: %s", path);
        return lvgl_image_handler_show_png_internal(path);

    case LVGL_IMAGE_HANDLER_FORMAT_GIF:
        ESP_LOGI(TAG, "Show GIF animation: %s", path);
        return lvgl_image_handler_show_gif_internal(path);

    default:
        ESP_LOGE(TAG, "Unsupported image format: %d", (int)format);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lvgl_image_handler_show_jpg(const char *path)
{
    return lvgl_image_handler_show(
        path,
        LVGL_IMAGE_HANDLER_FORMAT_JPG
    );
}

esp_err_t lvgl_image_handler_show_png(const char *path)
{
    return lvgl_image_handler_show(
        path,
        LVGL_IMAGE_HANDLER_FORMAT_PNG
    );
}

esp_err_t lvgl_image_handler_show_gif(const char *path)
{
    return lvgl_image_handler_show(
        path,
        LVGL_IMAGE_HANDLER_FORMAT_GIF
    );
}

esp_err_t lvgl_image_handler_clear(void)
{
    lvgl_image_handler_clear_internal();

    ESP_LOGI(TAG, "Active image object cleared");

    return ESP_OK;
}

bool lvgl_image_handler_has_active_object(void)
{
    return (s_active_object != NULL);
}
