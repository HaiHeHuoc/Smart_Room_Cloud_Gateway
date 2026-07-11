#include "lvgl_image_handler.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "lvgl.h"
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


/* Global Variables -------------------------------------------------------- */
/* Define file-scope Global variables here. */

/* Function Prototypes ----------------------------------------------------- */
/* Declare static helper functions here. */

/* Static Functions ------------------------------------------------------- */
/* Implement static helper functions here. */

static void lvgl_image_handler_clear_internal(void)
{
    if (s_active_object == NULL) {
        ESP_LOGI(TAG, "Available to display a new image");
        return;
    }

    /*
     * Delete the LVGL object and all resources owned by that object.
     *
     * Note:
     * The image decoder may still manage its own cache separately.
     */

    ESP_LOGI(TAG, "Delete the current image");
    lv_obj_delete(s_active_object);
    s_active_object = NULL;
}

static uint32_t lvgl_image_handler_calculate_contain_scale(
    uint32_t image_width,
    uint32_t image_height,
    uint32_t frame_width,
    uint32_t frame_height,
    bool allow_upscale)
{
    if ((image_width == 0U)  ||
        (image_height == 0U) ||
        (frame_width == 0U)  ||
        (frame_height == 0U)) {
        return LV_SCALE_NONE;
    }

    /*
     * LV_SCALE_NONE = 256 = 100%.
     *
     * uint64_t prevents overflow during:
     * frame_size * LV_SCALE_NONE.
     */
    const uint32_t scale_x =
        (uint32_t)(((uint64_t)frame_width * LV_SCALE_NONE) /
                   image_width);

    const uint32_t scale_y =
        (uint32_t)(((uint64_t)frame_height * LV_SCALE_NONE) /
                   image_height);

    uint32_t scale = (scale_x < scale_y)
                         ? scale_x
                         : scale_y;

    /*
     * Downscale only:
     * Do not enlarge an image that is already smaller than the frame.
     */
    if ((!allow_upscale) && (scale > LV_SCALE_NONE)) {
        scale = LV_SCALE_NONE;
    }

    /*
     * Avoid zero scale for an extremely large source image.
     */
    if (scale == 0U) {
        scale = 1U;
    }

    return scale;
}

/**
 * @brief Apply an already calculated contain scale.
 *
 * @note This only works when the image decoder supports transformations.
 *       Do not use it directly with LVGL TJPGD file sources.
 */
static void lvgl_image_handler_apply_contain_scale(
    lv_obj_t *image_obj,
    uint32_t scale)
{
    if (image_obj == NULL) {
        return;
    }

    lv_image_set_scale(image_obj, scale);
    lv_obj_center(image_obj);
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

    const uint32_t contain_scale =
        lvgl_image_handler_calculate_contain_scale(
            image_header.w,
            image_header.h,
            frame_width,
            frame_height,
            false
        );

    ESP_LOGI(
        TAG,
        "JPG: path=%s, image=%lux%lu, frame=%lux%lu, scale=%lu/256",
        path,
        (unsigned long)image_header.w,
        (unsigned long)image_header.h,
        (unsigned long)frame_width,
        (unsigned long)frame_height,
        (unsigned long)contain_scale
    );

    /*
     * 3. Create and validate the new LVGL image object.
     */
    lv_obj_t *new_image_obj = lv_image_create(screen);

    ESP_RETURN_ON_FALSE(
        new_image_obj != NULL,
        ESP_ERR_NO_MEM,
        TAG,
        "Failed to create LVGL image object"
    );

    /*
     * 4. Configure the JPG source.
     */
    lv_image_set_src(new_image_obj, path);

    /*
     * TJPGD decodes JPEG in tiles and does not support zoom/rotation.
     * Therefore, display this JPG at native resolution.
     */
    lv_obj_center(new_image_obj);

    /*
     * Do not enable this with TJPGD:
     *
     * lvgl_image_handler_apply_contain_scale(
     *     new_image_obj,
     *     contain_scale
     * );
     */

    /*
     * 5. Replace the previously managed image.
     */
    lv_obj_t *old_image_obj = s_active_object;
    s_active_object = new_image_obj;

    if (old_image_obj != NULL) {
        lv_obj_delete(old_image_obj);
    }

    ESP_LOGI(TAG, "JPG image object created successfully");

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
