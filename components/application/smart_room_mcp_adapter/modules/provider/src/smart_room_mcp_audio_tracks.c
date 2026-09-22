#include "smart_room_mcp_adapter_internal.h"
#include "smart_room_mcp_adapter.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "sd_card_manager.h"
#include "smart_room_mcp_audio_catalog_entry.h"
#include "voice_assistant_playback_control.h"
#include "xiaozhi_foundation.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SMART_ROOM_AUDIO_ROOT "/sdcard/audio"
#define SMART_ROOM_AUDIO_FILENAME_MAX_BYTES \
    (SD_CARD_MANAGER_DIRECTORY_ENTRY_NAME_MAX_LEN + 1U)
#define SMART_ROOM_AUDIO_CATALOG_TASK_NAME "mcp_audio_catalog"
#define SMART_ROOM_AUDIO_CATALOG_TASK_STACK 4096U
#define SMART_ROOM_AUDIO_CATALOG_TASK_PRIORITY 3U
#define SMART_ROOM_AUDIO_CATALOG_LOCK_MS 25U
#define SMART_ROOM_AUDIO_CATALOG_MOUNT_RETRY_MS 2000U

typedef struct {
    xiaozhi_foundation_audio_track_t public_track;
    char filename[SMART_ROOM_AUDIO_FILENAME_MAX_BYTES];
} smart_room_catalog_entry_t;

typedef struct {
    bool available;
    bool truncated;
    uint8_t count;
    smart_room_catalog_entry_t entries[XIAOZHI_FOUNDATION_AUDIO_TRACK_MAX_COUNT];
} smart_room_catalog_t;

static const char *const TAG = "MCP_AUDIO_TRACKS";
/* The catalog is built exclusively by this low-priority task. MCP callbacks
 * only take the mutex with a zero wait and copy/look up the published fixed
 * snapshot; they never acquire an SD lease or perform filesystem I/O on the
 * Xiaozhi WebSocket dispatch stack. */
static SemaphoreHandle_t s_catalog_lock = NULL;
static TaskHandle_t s_catalog_task = NULL;
/* A committed storage mutation increments requested. Readers must never copy
 * a snapshot until the worker has scanned and published that same epoch. */
static volatile uint32_t s_catalog_requested_epoch;
static volatile uint32_t s_catalog_published_epoch;
/* The snapshot is neither DMA/ISR-visible nor used while flash cache is
 * disabled. Keeping it in PSRAM preserves Internal RAM for TLS, I2S, and
 * transport control paths. */
EXT_RAM_BSS_ATTR static smart_room_catalog_t s_catalog;

static bool make_entry(const char *filename, smart_room_catalog_entry_t *entry)
{
    if ((filename == NULL) || (entry == NULL)) {
        return false;
    }
    *entry = (smart_room_catalog_entry_t){0};
    return smart_room_mcp_audio_catalog_entry_prepare(
        filename,
        entry->public_track.id,
        sizeof(entry->public_track.id),
        entry->public_track.name,
        sizeof(entry->public_track.name),
        entry->filename,
        sizeof(entry->filename));
}

static bool catalog_contains_track_id(const smart_room_catalog_t *catalog,
                                      const char *track_id,
                                      const smart_room_catalog_entry_t *ignored)
{
    if ((catalog == NULL) || (track_id == NULL)) {
        return false;
    }
    for (uint8_t index = 0U; index < catalog->count; ++index) {
        if (&catalog->entries[index] == ignored) {
            continue;
        }
        if (strcmp(catalog->entries[index].public_track.id, track_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool make_catalog_track_id_unique(smart_room_catalog_t *catalog,
                                         smart_room_catalog_entry_t *entry)
{
    if ((catalog == NULL) || (entry == NULL) ||
        !catalog_contains_track_id(catalog, entry->public_track.id, entry)) {
        return true;
    }

    char base_id[XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES] = {0};
    const size_t initial_length = strnlen(
        entry->public_track.id, sizeof(entry->public_track.id));
    if (initial_length >= sizeof(base_id)) {
        return false;
    }
    memcpy(base_id, entry->public_track.id, initial_length + 1U);

    for (uint8_t suffix = 2U;
         suffix <= XIAOZHI_FOUNDATION_AUDIO_TRACK_MAX_COUNT;
         ++suffix) {
        const int id_result = snprintf(
            entry->public_track.id, sizeof(entry->public_track.id), "%s-%u",
            base_id, (unsigned)suffix);
        if ((id_result < 0) ||
            ((size_t)id_result >= sizeof(entry->public_track.id))) {
            return false;
        }
        if (!catalog_contains_track_id(catalog, entry->public_track.id, entry)) {
            return true;
        }
    }
    return false;
}

static bool catalog_assign_unique_track_ids(smart_room_catalog_t *catalog)
{
    if (catalog == NULL) {
        return false;
    }

    /* insert_entry() has already placed retained files in lexical filename
     * order. Preserve their base candidates, clear the live IDs, then assign
     * collisions in that order so FAT readdir order cannot change an ID. */
    char base_ids[XIAOZHI_FOUNDATION_AUDIO_TRACK_MAX_COUNT]
                 [XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES] = {{0}};
    for (uint8_t index = 0U; index < catalog->count; ++index) {
        const size_t length = strnlen(
            catalog->entries[index].public_track.id,
            sizeof(catalog->entries[index].public_track.id));
        if (length >= sizeof(base_ids[index])) {
            return false;
        }
        memcpy(base_ids[index],
               catalog->entries[index].public_track.id,
               length + 1U);
        memset(catalog->entries[index].public_track.id,
               0,
               sizeof(catalog->entries[index].public_track.id));
    }

    for (uint8_t index = 0U; index < catalog->count; ++index) {
        memcpy(catalog->entries[index].public_track.id,
               base_ids[index],
               sizeof(catalog->entries[index].public_track.id));
        if (!make_catalog_track_id_unique(catalog, &catalog->entries[index])) {
            return false;
        }
    }
    return true;
}

static void insert_entry(smart_room_catalog_t *catalog,
                         smart_room_catalog_entry_t *entry)
{
    if ((catalog == NULL) || (entry == NULL)) {
        return;
    }
    uint8_t index = 0U;
    while ((index < catalog->count) &&
           (strcmp(catalog->entries[index].filename,
                   entry->filename) < 0)) ++index;
    if ((index < catalog->count) &&
        (strcmp(catalog->entries[index].filename,
                entry->filename) == 0)) return;
    if (catalog->count < XIAOZHI_FOUNDATION_AUDIO_TRACK_MAX_COUNT) {
        for (uint8_t i = catalog->count; i > index; --i)
            catalog->entries[i] = catalog->entries[i - 1U];
        catalog->entries[index] = *entry;
        ++catalog->count;
        return;
    }
    catalog->truncated = true;
    if (index >= catalog->count) return;
    for (uint8_t i = catalog->count - 1U; i > index; --i)
        catalog->entries[i] = catalog->entries[i - 1U];
    catalog->entries[index] = *entry;
}

static esp_err_t scan_catalog(smart_room_catalog_t *catalog)
{
    if (catalog == NULL) return ESP_ERR_INVALID_ARG;
    *catalog = (smart_room_catalog_t){0};
    if (!sd_card_manager_is_mounted()) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = sd_card_manager_acquire();
    if (ret != ESP_OK) return ret;
    DIR *directory = opendir(SMART_ROOM_AUDIO_ROOT);
    if (directory == NULL) {
        const int open_error = errno;
        sd_card_manager_release();
        if (sd_card_manager_is_vfs_media_error(open_error)) {
            sd_card_manager_report_io_error(ESP_FAIL);
        }
        return ESP_FAIL;
    }
    catalog->available = true;
    int read_error = 0;
    for (;;) {
        errno = 0;
        struct dirent *node = readdir(directory);
        if (node == NULL) {
            read_error = errno;
            break;
        }
        smart_room_catalog_entry_t entry = {0};
        if (!make_entry(node->d_name, &entry)) continue;
        char path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
        const int path_length = snprintf(path, sizeof(path), "%s/%s",
                                         SMART_ROOM_AUDIO_ROOT, entry.filename);
        if ((path_length < 0) || ((size_t)path_length >= sizeof(path))) continue;
        struct stat info = {0};
        if (stat(path, &info) != 0) {
            const int stat_error = errno;
            if (sd_card_manager_is_vfs_media_error(stat_error)) {
                const int close_result = closedir(directory);
                const int close_error = errno;
                sd_card_manager_release();
                if (sd_card_manager_is_vfs_media_error(close_error) ||
                    (close_result != 0) ||
                    sd_card_manager_is_vfs_media_error(stat_error)) {
                    sd_card_manager_report_io_error(ESP_FAIL);
                }
                *catalog = (smart_room_catalog_t){0};
                return ESP_FAIL;
            }
            continue;
        }
        if (!S_ISREG(info.st_mode)) continue;
        _Static_assert(sizeof(info.st_size) == sizeof(int32_t),
                       "FATFS VFS file size conversion expects 32-bit off_t");
        entry.public_track.size_bytes =
            smart_room_mcp_audio_catalog_fatfs_size_bytes((int32_t)info.st_size);
        insert_entry(catalog, &entry);
    }
    const int close_result = closedir(directory);
    const int close_error = errno;
    sd_card_manager_release();
    if ((read_error != 0) || (close_result != 0)) {
        if (sd_card_manager_is_vfs_media_error(read_error) ||
            sd_card_manager_is_vfs_media_error(close_error)) {
            sd_card_manager_report_io_error(ESP_FAIL);
        }
        *catalog = (smart_room_catalog_t){0};
        return ESP_FAIL;
    }
    if (!catalog_assign_unique_track_ids(catalog)) {
        APP_LOGW(TAG, AUDIO_TRACK_ID_COLLISION_REJECTED_DDEB7CF4,
                 "audio catalog rejected an unresolved logical id collision");
        *catalog = (smart_room_catalog_t){0};
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void catalog_request_refresh(void)
{
    if (s_catalog_task != NULL) {
        xTaskNotifyGive(s_catalog_task);
    }
}

static bool catalog_lock(TickType_t wait_ticks)
{
    return (s_catalog_lock != NULL) &&
           (xSemaphoreTake(s_catalog_lock, wait_ticks) == pdTRUE);
}

static bool catalog_snapshot_is_current(void)
{
    return __atomic_load_n(&s_catalog_requested_epoch, __ATOMIC_RELAXED) ==
           __atomic_load_n(&s_catalog_published_epoch, __ATOMIC_RELAXED);
}

static bool catalog_refresh_once(void)
{
    const uint32_t requested_epoch =
        __atomic_load_n(&s_catalog_requested_epoch, __ATOMIC_RELAXED);
    if (!catalog_lock(pdMS_TO_TICKS(SMART_ROOM_AUDIO_CATALOG_LOCK_MS))) {
        return false;
    }

    const esp_err_t result = scan_catalog(&s_catalog);
    const bool available = (result == ESP_OK) && s_catalog.available;
    if (!available) {
        s_catalog = (smart_room_catalog_t){0};
    }
    __atomic_store_n(&s_catalog_published_epoch, requested_epoch,
                     __ATOMIC_RELAXED);
    xSemaphoreGive(s_catalog_lock);
    return available;
}

static void catalog_task(void *argument)
{
    (void)argument;
    for (;;) {
        const bool catalog_ready = catalog_refresh_once();
        (void)ulTaskNotifyTake(
            pdTRUE,
            catalog_ready
                ? portMAX_DELAY
                : pdMS_TO_TICKS(SMART_ROOM_AUDIO_CATALOG_MOUNT_RETRY_MS));
    }
}

static esp_err_t catalog_worker_start(void)
{
    if (s_catalog_lock == NULL) {
        s_catalog_lock = xSemaphoreCreateMutex();
        if (s_catalog_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_catalog_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreateWithCaps(
            catalog_task,
            SMART_ROOM_AUDIO_CATALOG_TASK_NAME,
            SMART_ROOM_AUDIO_CATALOG_TASK_STACK,
            NULL,
            SMART_ROOM_AUDIO_CATALOG_TASK_PRIORITY,
            &s_catalog_task,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_catalog_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    APP_LOGI(TAG, AUDIO_TRACK_CATALOG_WORKER_STAR_9FA3E509,
             "audio catalog worker started stack=%u priority=%u",
             (unsigned)SMART_ROOM_AUDIO_CATALOG_TASK_STACK,
             (unsigned)SMART_ROOM_AUDIO_CATALOG_TASK_PRIORITY);
    return ESP_OK;
}

static esp_err_t copy_track_list(xiaozhi_foundation_audio_track_list_t *tracks,
                                 void *context)
{
    (void)context;
    if (tracks == NULL) return ESP_ERR_INVALID_ARG;
    *tracks = (xiaozhi_foundation_audio_track_list_t){0};
    catalog_request_refresh();
    if (!sd_card_manager_is_mounted()) return ESP_ERR_INVALID_STATE;
    /* Never wait on the WebSocket dispatch path. A concurrent scan is
     * reported as temporarily unavailable and the caller can retry. */
    if (!catalog_lock(0U)) return ESP_ERR_TIMEOUT;
    if (!s_catalog.available || !catalog_snapshot_is_current()) {
        xSemaphoreGive(s_catalog_lock);
        return ESP_ERR_INVALID_STATE;
    }
    tracks->available = true;
    tracks->truncated = s_catalog.truncated;
    tracks->track_count = s_catalog.count;
    for (uint8_t i = 0U; i < s_catalog.count; ++i) {
        tracks->tracks[i] = s_catalog.entries[i].public_track;
    }
    xSemaphoreGive(s_catalog_lock);
    return ESP_OK;
}

static esp_err_t play_track(const char *track_id,
                            xiaozhi_foundation_audio_track_play_result_t *result,
                            void *context)
{
    (void)context;
    if ((track_id == NULL) || (result == NULL)) return ESP_ERR_INVALID_ARG;
    *result = (xiaozhi_foundation_audio_track_play_result_t){
        .outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_INTERNAL_ERROR,
    };
    const size_t id_length = strnlen(
        track_id, XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES);
    if ((id_length == 0U) ||
        (id_length >= XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES) ||
        (strstr(track_id, "..") != NULL) || (strchr(track_id, '/') != NULL) ||
        (strchr(track_id, '\\') != NULL) || (strchr(track_id, '.') != NULL)) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_INVALID_REQUEST;
        return ESP_OK;
    }

    catalog_request_refresh();
    if (!sd_card_manager_is_mounted()) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_STORAGE_UNAVAILABLE;
        return ESP_OK;
    }
    if (!catalog_lock(0U)) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_CATALOG_UNAVAILABLE;
        return ESP_OK;
    }

    char path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
    bool found = false;
    if (s_catalog.available && catalog_snapshot_is_current()) {
        for (uint8_t i = 0U; i < s_catalog.count; ++i) {
            if (strcmp(track_id, s_catalog.entries[i].public_track.id) != 0) {
                continue;
            }
            const int length = snprintf(
                path, sizeof(path), "%s/%s", SMART_ROOM_AUDIO_ROOT,
                s_catalog.entries[i].filename);
            found = (length >= 0) && ((size_t)length < sizeof(path));
            break;
        }
    }
    const bool catalog_available =
        s_catalog.available && catalog_snapshot_is_current();
    xSemaphoreGive(s_catalog_lock);

    if (!catalog_available) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_CATALOG_UNAVAILABLE;
        return ESP_OK;
    }
    if (!found) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_NOT_FOUND;
        return ESP_OK;
    }

    const esp_err_t play_ret = voice_assistant_playback_start_catalog_wav(path);
    if (play_ret == ESP_OK) {
        /* The arbiter has accepted a request only. It will open the WAV and
         * acquire I2S later, so callers must not interpret this as proof that
         * sound has already reached the speaker. */
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS;
        result->accepted = true;
        result->scheduled = true;
    } else {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_PLAYBACK_REJECTED;
    }
    return ESP_OK;
}

static esp_err_t play_recorded(
    xiaozhi_foundation_audio_track_play_result_t *result, void *context)
{
    (void)context;
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    *result = (xiaozhi_foundation_audio_track_play_result_t){
        .outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_INTERNAL_ERROR,
    };
    audio_manager_status_t status = {0};
    if ((audio_manager_get_status(&status) != ESP_OK) ||
        !status.recorded_audio_available) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_RECORDED_AUDIO_NOT_AVAILABLE;
        return ESP_OK;
    }
    const esp_err_t ret = voice_assistant_playback_start_recorded();
    if (ret == ESP_OK) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS;
        result->accepted = true;
        result->scheduled = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_PLAYBACK_REJECTED;
    }
    return ESP_OK;
}

esp_err_t smart_room_mcp_adapter_audio_catalog_get(
    smart_room_audio_catalog_t *catalog)
{
    if (catalog == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *catalog = (smart_room_audio_catalog_t){0};
    catalog_request_refresh();
    if (!sd_card_manager_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!catalog_lock(0U)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_catalog.available || !catalog_snapshot_is_current()) {
        xSemaphoreGive(s_catalog_lock);
        return ESP_ERR_INVALID_STATE;
    }
    catalog->available = true;
    catalog->truncated = s_catalog.truncated;
    catalog->track_count = s_catalog.count;
    for (uint8_t index = 0U; index < s_catalog.count; ++index) {
        memcpy(catalog->tracks[index].id, s_catalog.entries[index].public_track.id,
               sizeof(catalog->tracks[index].id));
        memcpy(catalog->tracks[index].name, s_catalog.entries[index].public_track.name,
               sizeof(catalog->tracks[index].name));
        memcpy(catalog->tracks[index].filename, s_catalog.entries[index].filename,
               sizeof(catalog->tracks[index].filename));
        catalog->tracks[index].size_bytes = s_catalog.entries[index].public_track.size_bytes;
    }
    xSemaphoreGive(s_catalog_lock);
    return ESP_OK;
}

void smart_room_mcp_adapter_audio_catalog_invalidate(void)
{
    uint32_t next_epoch = __atomic_add_fetch(
        &s_catalog_requested_epoch, 1U, __ATOMIC_RELAXED);
    if (next_epoch == 0U) {
        __atomic_store_n(&s_catalog_requested_epoch, 1U, __ATOMIC_RELAXED);
    }
    catalog_request_refresh();
}

esp_err_t smart_room_mcp_adapter_audio_catalog_play(
    const char *track_id,
    smart_room_audio_catalog_play_result_t *result)
{
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xiaozhi_foundation_audio_track_play_result_t play = {0};
    const esp_err_t ret = play_track(track_id, &play, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    *result = (smart_room_audio_catalog_play_result_t){
        .accepted = play.accepted,
        .scheduled = play.scheduled,
        .outcome = SMART_ROOM_AUDIO_CATALOG_PLAY_INTERNAL_ERROR,
    };
    switch (play.outcome) {
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS:
            result->outcome = SMART_ROOM_AUDIO_CATALOG_PLAY_SUCCESS;
            break;
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_INVALID_REQUEST:
            result->outcome = SMART_ROOM_AUDIO_CATALOG_PLAY_INVALID_REQUEST;
            break;
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_STORAGE_UNAVAILABLE:
            result->outcome = SMART_ROOM_AUDIO_CATALOG_PLAY_STORAGE_UNAVAILABLE;
            break;
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_CATALOG_UNAVAILABLE:
            result->outcome = SMART_ROOM_AUDIO_CATALOG_PLAY_UNAVAILABLE;
            break;
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_NOT_FOUND:
            result->outcome = SMART_ROOM_AUDIO_CATALOG_PLAY_NOT_FOUND;
            break;
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_PLAYBACK_REJECTED:
            result->outcome = SMART_ROOM_AUDIO_CATALOG_PLAY_REJECTED;
            break;
        case XIAOZHI_FOUNDATION_AUDIO_TRACK_INTERNAL_ERROR:
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t smart_room_mcp_audio_tracks_register_providers(void)
{
    esp_err_t ret = catalog_worker_start();
    if (ret != ESP_OK) return ret;
    ret = xiaozhi_foundation_register_audio_track_list_provider(
        copy_track_list, NULL);
    if (ret != ESP_OK) return ret;
    ret = xiaozhi_foundation_register_audio_track_play_provider(play_track, NULL);
    if (ret != ESP_OK) return ret;
    return xiaozhi_foundation_register_audio_recorded_play_provider(
        play_recorded, NULL);
}
