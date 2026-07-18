/* Includes ----------------------------------------------------------------- */
#include "lvgl_sd_fs.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"

#include "board_config.h"
#include "sd_card_manager.h"

#include "lvgl.h"

/* Macros ------------------------------------------------------------------- */
#define LVGL_SD_FS_LETTER 'S'
#define LVGL_SD_FS_PATH_MAX_LEN SD_CARD_MANAGER_PATH_MAX_LEN

/* Constants ---------------------------------------------------------------- */
static const char *const TAG = "LVGL_SD_FS";

/* Static Variables --------------------------------------------------------- */
static lv_fs_drv_t s_lvgl_sd_fs_drv;
static bool s_lvgl_sd_fs_registered = false;

/* Function Prototypes ------------------------------------------------------ */
static bool lvgl_sd_fs_build_full_path(
    const char *lvgl_path,
    char *out_path,
    size_t out_path_size);
static bool lvgl_sd_fs_ready_cb(lv_fs_drv_t *drv);
static void *lvgl_sd_fs_open_cb(
    lv_fs_drv_t *drv,
    const char *path,
    lv_fs_mode_t mode);
static lv_fs_res_t lvgl_sd_fs_close_cb(
    lv_fs_drv_t *drv,
    void *file_p);
static lv_fs_res_t lvgl_sd_fs_read_cb(
    lv_fs_drv_t *drv,
    void *file_p,
    void *buf,
    uint32_t btr,
    uint32_t *br);
static lv_fs_res_t lvgl_sd_fs_seek_cb(
    lv_fs_drv_t *drv,
    void *file_p,
    uint32_t pos,
    lv_fs_whence_t whence);
static lv_fs_res_t lvgl_sd_fs_tell_cb(
    lv_fs_drv_t *drv,
    void *file_p,
    uint32_t *pos_p);

/* Static Functions --------------------------------------------------------- */
/**
 * @brief Convert an LVGL relative file path into an SD card VFS path.
 *
 * LVGL removes the drive prefix before calling open_cb(). For example,
 * "S:/images/logo.bin" reaches this helper as "/images/logo.bin".
 *
 * This helper maps it to:
 *
 *      "/sdcard/images/logo.bin"
 *
 * @param lvgl_path Path received from LVGL after the drive letter.
 * @param out_path Buffer that receives the full VFS path.
 * @param out_path_size Size of out_path in bytes.
 * @return true if the path was built successfully, false otherwise.
 */
static bool lvgl_sd_fs_build_full_path(const char *lvgl_path,
                                       char *out_path,
                                       size_t out_path_size)
{
    if (lvgl_path == NULL || out_path == NULL || out_path_size == 0)
    {
        return false;
    }

    int written = 0;

    if (lvgl_path[0] == '/')
    {
        written = snprintf(out_path,
                           out_path_size,
                           "%s%s",
                           SD_MOUNT_POINT,
                           lvgl_path);
    }
    else
    {
        written = snprintf(out_path,
                           out_path_size,
                           "%s/%s",
                           SD_MOUNT_POINT,
                           lvgl_path);
    }

    if (written < 0 || written >= (int)out_path_size)
    {
        return false;
    }

    return true;
}

/**
 * @brief Tell LVGL whether this filesystem is currently available.
 *
 * LVGL may call this before file operations. The SD card manager owns the real
 * mount state, so this callback simply reflects that state.
 *
 * @param drv LVGL filesystem driver pointer.
 * @return true if the SD card is mounted, false otherwise.
 */
static bool lvgl_sd_fs_ready_cb(lv_fs_drv_t *drv)
{
    (void)drv;

    return sd_card_manager_is_mounted();
}

/**
 * @brief Open a file from the SD card for LVGL.
 *
 * LVGL passes paths without the "S:" drive prefix. This callback converts that
 * path to the ESP-IDF VFS path under SD_MOUNT_POINT and opens it using stdio.
 *
 * The returned FILE pointer becomes LVGL's opaque file handle and is passed
 * back to close/read/seek/tell callbacks.
 *
 * @param drv LVGL filesystem driver pointer.
 * @param path LVGL path after removing the drive prefix.
 * @param mode LVGL read/write mode flags.
 * @return FILE pointer on success, NULL on failure.
 */
static void *lvgl_sd_fs_open_cb(lv_fs_drv_t *drv,
                                const char *path,
                                lv_fs_mode_t mode)
{
    (void)drv;

    if (!sd_card_manager_is_mounted())
    {
        ESP_LOGE(TAG, "SD card is not mounted");
        return NULL;
    }

    char full_path[LVGL_SD_FS_PATH_MAX_LEN] = {0};

    if (!lvgl_sd_fs_build_full_path(path, full_path, sizeof(full_path)))
    {
        ESP_LOGE(TAG, "Failed to build full path for LVGL path: %s", path);
        return NULL;
    }

    const char *mode_str = NULL;

    /*
     * Convert LVGL open mode to C stdio open mode.
     *
     * LV_FS_MODE_RD -> "rb"
     * LV_FS_MODE_WR -> "wb"
     * RD + WR       -> "rb+"
     */
    if ((mode & LV_FS_MODE_WR) && (mode & LV_FS_MODE_RD))
    {
        mode_str = "rb+";
    }
    else if (mode & LV_FS_MODE_WR)
    {
        mode_str = "wb";
    }
    else if (mode & LV_FS_MODE_RD)
    {
        mode_str = "rb";
    }
    else
    {
        ESP_LOGE(TAG, "Unsupported LVGL file open mode");
        return NULL;
    }

    FILE *file = fopen(full_path, mode_str);
    if (file == NULL)
    {
        ESP_LOGE(TAG,
                 "Failed to open file: %s, errno: %d",
                 full_path,
                 errno);
        return NULL;
    }

    ESP_LOGD(TAG, "Opened file: %s", full_path);

    return file;
}

/**
 * @brief Close a stdio file handle opened by lvgl_sd_fs_open_cb().
 *
 * @param drv LVGL filesystem driver pointer.
 * @param file_p Opaque file pointer previously returned by open_cb.
 * @return LV_FS_RES_OK on success, LV_FS_RES_FS_ERR on failure.
 */
static lv_fs_res_t lvgl_sd_fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;

    FILE *file = (FILE *)file_p;

    if (file == NULL)
    {
        return LV_FS_RES_FS_ERR;
    }

    int ret = fclose(file);

    return (ret == 0) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

/**
 * @brief Read file callback for LVGL filesystem.
 *
 * LVGL uses this when decoding images, fonts, GIFs, or other file-backed
 * assets. The function reads raw bytes from the stdio file handle.
 *
 * @param drv LVGL filesystem driver pointer.
 * @param file_p Opaque file pointer previously returned by open_cb.
 * @param buf Destination buffer owned by LVGL.
 * @param btr Number of bytes LVGL wants to read.
 * @param br Number of bytes actually read.
 * @return LV_FS_RES_OK on success, LV_FS_RES_FS_ERR on failure.
 */
static lv_fs_res_t lvgl_sd_fs_read_cb(lv_fs_drv_t *drv,
                                      void *file_p,
                                      void *buf,
                                      uint32_t btr,
                                      uint32_t *br)
{
    (void)drv;

    FILE *file = (FILE *)file_p;

    if (file == NULL || buf == NULL)
    {
        return LV_FS_RES_FS_ERR;
    }

    size_t read_count = fread(buf, 1, btr, file);

    if (br != NULL)
    {
        *br = (uint32_t)read_count;
    }

    /*
     * If fread reads less than requested, it may be either:
     *
     * 1. End of file
     * 2. Real file read error
     *
     * ferror() tells us if it is a real error.
     */
    if (read_count < btr && ferror(file))
    {
        clearerr(file);
        return LV_FS_RES_FS_ERR;
    }

    return LV_FS_RES_OK;
}

/**
 * @brief Seek file callback for LVGL filesystem.
 *
 * LVGL uses this to move the file pointer.
 *
 * @param drv LVGL filesystem driver pointer.
 * @param file_p Opaque file pointer previously returned by open_cb.
 * @param pos Offset value.
 * @param whence Seek base, such as start/current/end.
 * @return LV_FS_RES_OK on success, LV_FS_RES_FS_ERR on failure.
 */
static lv_fs_res_t lvgl_sd_fs_seek_cb(lv_fs_drv_t *drv,
                                      void *file_p,
                                      uint32_t pos,
                                      lv_fs_whence_t whence)
{
    (void)drv;

    FILE *file = (FILE *)file_p;

    if (file == NULL)
    {
        return LV_FS_RES_FS_ERR;
    }

    int origin = SEEK_SET;

    switch (whence)
    {
    case LV_FS_SEEK_SET:
        origin = SEEK_SET;
        break;

    case LV_FS_SEEK_CUR:
        origin = SEEK_CUR;
        break;

    case LV_FS_SEEK_END:
        origin = SEEK_END;
        break;

    default:
        return LV_FS_RES_FS_ERR;
    }

    int ret = fseek(file, (long)pos, origin);

    return (ret == 0) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

/**
 * @brief Tell file position callback for LVGL filesystem.
 *
 * This returns current file position.
 *
 * @param drv LVGL filesystem driver pointer.
 * @param file_p Opaque file pointer previously returned by open_cb.
 * @param pos_p Output current file position.
 * @return LV_FS_RES_OK on success, LV_FS_RES_FS_ERR on failure.
 */
static lv_fs_res_t lvgl_sd_fs_tell_cb(lv_fs_drv_t *drv,
                                      void *file_p,
                                      uint32_t *pos_p)
{
    (void)drv;

    FILE *file = (FILE *)file_p;

    if (file == NULL || pos_p == NULL)
    {
        return LV_FS_RES_FS_ERR;
    }

    long pos = ftell(file);

    if (pos < 0)
    {
        return LV_FS_RES_FS_ERR;
    }

    *pos_p = (uint32_t)pos;

    return LV_FS_RES_OK;
}

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Register SD card filesystem driver to LVGL.
 *
 * Must be called after:
 *
 *      1. lv_init()
 *      2. sd_card_manager_init()
 *
 * After this function, LVGL can access SD card files using:
 *
 *      S:/file_name
 */
esp_err_t lvgl_sd_fs_register(void)
{
    if (s_lvgl_sd_fs_registered)
    {
        ESP_LOGW(TAG, "LVGL SD file system driver already registered");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Registering SD card filesystem driver with LVGL");

    ESP_RETURN_ON_FALSE(sd_card_manager_is_mounted(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "SD card must be mounted before registering LVGL FS driver");

    lv_fs_drv_init(&s_lvgl_sd_fs_drv);

    /*
     * LVGL paths will use this drive letter. Example:
     *      S:/images/logo.png
     */
    s_lvgl_sd_fs_drv.letter = LVGL_SD_FS_LETTER;

    /*
     * Keep LVGL FS cache disabled for now. This is simpler for bring-up and
     * avoids extra RAM usage while the SD card path is still being validated.
     */
    s_lvgl_sd_fs_drv.cache_size = 0;

    /*
     * Register only the operations needed for read-only LVGL assets today.
     * Write mode is mapped in open_cb, but there are no write/remove/rename
     * callbacks yet.
     */
    s_lvgl_sd_fs_drv.ready_cb = lvgl_sd_fs_ready_cb;
    s_lvgl_sd_fs_drv.open_cb = lvgl_sd_fs_open_cb;
    s_lvgl_sd_fs_drv.close_cb = lvgl_sd_fs_close_cb;
    s_lvgl_sd_fs_drv.read_cb = lvgl_sd_fs_read_cb;
    s_lvgl_sd_fs_drv.seek_cb = lvgl_sd_fs_seek_cb;
    s_lvgl_sd_fs_drv.tell_cb = lvgl_sd_fs_tell_cb;

    ESP_LOGD(TAG, "Before calling lv_fs_drv_register");

    /*
     * Register the driver to LVGL.
     */
    lv_fs_drv_register(&s_lvgl_sd_fs_drv);

    s_lvgl_sd_fs_registered = true;

    ESP_LOGI(TAG,
             "LVGL SD filesystem registered as %c:",
             LVGL_SD_FS_LETTER);

    return ESP_OK;
}

bool lvgl_sd_fs_is_registered(void)
{
    return s_lvgl_sd_fs_registered;
}

bool lvgl_sd_fs_is_ready(void)
{
    return s_lvgl_sd_fs_registered && sd_card_manager_is_mounted();
}
