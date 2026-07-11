#include "lvgl_image_handler.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"
#include "lvgl_sd_fs.h"


/* Macros ------------------------------------------------------------------ */
/* Define event bits, GPIO pins, task stack sizes, priorities, etc. here. */

/* Constants --------------------------------------------------------------- */
/* Define file-scope const values here. */
static const char* TAG = "LVGL_IMAGE_HANDLER";

/* Type Definitions -------------------------------------------------------- */
/* Define local enums, structs, and typedefs here. */

/* Static Variables -------------------------------------------------------- */
/* Define file-scope static variables here. */
static lv_obj_t *s_active_object = NULL;
static lv_draw_buf_t *s_active_draw_buf = NULL;


/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

static void lvgl_image_handler_clear_internal(void)
{
    if ((s_active_object == NULL) && (s_active_draw_buf == NULL)) {
        ESP_LOGI(TAG, "Available to display a new image");
        return;
    }

    ESP_LOGI(TAG, "Delete the current image");

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
                        "Invalid JPEG decoder data");

    const lv_draw_buf_t *decoded = decoder_dsc->decoded;

    ESP_RETURN_ON_FALSE(decoded != NULL && decoded->data != NULL,
                        ESP_FAIL,
                        TAG,
                        "JPEG decoder returned no pixel data");

    ESP_RETURN_ON_FALSE(decoded->header.cf == LV_COLOR_FORMAT_RGB888,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "Unsupported decoded JPEG color format: %u",
                        (unsigned int)decoded->header.cf);

    ESP_RETURN_ON_FALSE(decoded_area->x1 >= 0 && decoded_area->y1 >= 0 &&
                        decoded_area->x2 >= decoded_area->x1 &&
                        decoded_area->y2 >= decoded_area->y1,
                        ESP_FAIL,
                        TAG,
                        "Invalid decoded JPEG area");

    const uint32_t area_width = (uint32_t)lv_area_get_width(decoded_area);
    const uint32_t area_height = (uint32_t)lv_area_get_height(decoded_area);

    ESP_RETURN_ON_FALSE(decoded->header.w == area_width &&
                        decoded->header.h == area_height &&
                        decoded->header.stride >= (area_width * 3U),
                        ESP_FAIL,
                        TAG,
                        "JPEG decoder area and pixel buffer do not match");

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
                source_data + (source_column * 3U);

            /* TJPGD stores LV_COLOR_FORMAT_RGB888 pixels as B, G, R bytes. */
            const uint8_t blue = source_pixel[0];
            const uint8_t green = source_pixel[1];
            const uint8_t red = source_pixel[2];
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

static esp_err_t lvgl_image_handler_decode_jpg_to_frame(
    const char *path,
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

    if (lv_image_decoder_open(&decoder_dsc, path, NULL) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to open JPG decoder for: %s", path);
        return ESP_FAIL;
    }

    if ((decoder_dsc.header.w != source_width) ||
        (decoder_dsc.header.h != source_height)) {
        ESP_LOGE(TAG,
                 "JPG size changed while opening decoder: expected=%lux%lu, actual=%ux%u",
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
        ESP_LOGE(TAG, "JPG decoding ended before the complete image was read");
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

static esp_err_t lvgl_image_handler_show_jpg_internal(const char *path)
{
#if LV_USE_TJPGD

    ESP_RETURN_ON_FALSE(
        path != NULL && path[0] != '\0',
        ESP_ERR_INVALID_ARG,
        TAG,
        "Invalid JPG path"
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
     * 2. Read JPG header through the registered decoder.
     */
    lv_image_header_t image_header = {0};

    const lv_result_t result =
        lv_image_decoder_get_info(path, &image_header);

    ESP_RETURN_ON_FALSE(
        result == LV_RESULT_OK,
        ESP_FAIL,
        TAG,
        "No LVGL decoder can read JPG file: %s",
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
        "JPG: path=%s, source=%lux%lu, frame=%lux%lu, scaled=%lux%lu",
        path,
        (unsigned long)image_header.w,
        (unsigned long)image_header.h,
        (unsigned long)frame_width,
        (unsigned long)frame_height,
        (unsigned long)scaled_width,
        (unsigned long)scaled_height
    );

    /*
     * TJPGD supplies JPEG pixels to LVGL one MCU block at a time. LVGL's
     * generic image transform cannot reliably scale those independent blocks.
     * Decode and resize them into one screen-sized RGB565 buffer first, then
     * display that buffer without another transform.
     */
    lv_draw_buf_t *new_draw_buf = NULL;

    ESP_RETURN_ON_ERROR(
        lvgl_image_handler_decode_jpg_to_frame(path,
                                               image_header.w,
                                               image_header.h,
                                               frame_width,
                                               frame_height,
                                               scaled_width,
                                               scaled_height,
                                               &new_draw_buf),
        TAG,
        "Failed to decode and scale JPG"
    );

    /*
     * 3. Create and validate the new LVGL image object.
     */
    lv_obj_t *new_image_obj = lv_image_create(screen);

    if (new_image_obj == NULL) {
        lv_draw_buf_destroy(new_draw_buf);
        ESP_LOGE(TAG, "Failed to create LVGL image object");
        return ESP_ERR_NO_MEM;
    }

    /*
     * 4. The prepared buffer already has the screen dimensions and black bars
     * where required, so no LVGL transform is needed here.
     */
    lv_image_set_src(new_image_obj, new_draw_buf);
    lv_obj_set_size(new_image_obj, (int32_t)frame_width, (int32_t)frame_height);
    lv_obj_center(new_image_obj);

    /*
     * 5. Replace the previously managed image.
     */
    lv_obj_t *old_image_obj = s_active_object;
    lv_draw_buf_t *old_draw_buf = s_active_draw_buf;
    s_active_object = new_image_obj;
    s_active_draw_buf = new_draw_buf;

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
             "JPG image displayed from %lu-byte RGB565 frame",
             (unsigned long)new_draw_buf->data_size);

    return ESP_OK;

#else

    ESP_LOGE(
        TAG,
        "JPG support is disabled because LV_USE_TJPGD is not enabled"
    );

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
        ESP_LOGI(TAG, "TODO: show JPG image: %s", path);
        return lvgl_image_handler_show_jpg_internal(path);

    case LVGL_IMAGE_HANDLER_FORMAT_PNG:
        ESP_LOGI(TAG, "TODO: show PNG image: %s", path);
        break;

    case LVGL_IMAGE_HANDLER_FORMAT_GIF:
        ESP_LOGI(TAG, "TODO: show GIF animation: %s", path);
        break;

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
