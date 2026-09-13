#include "smart_room_mcp_adapter_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sd_card_manager.h"
#include "voice_assistant_playback_control.h"
#include "xiaozhi_foundation.h"

#define SMART_ROOM_AUDIO_ROOT "/sdcard/audio"
#define SMART_ROOM_AUDIO_FILENAME_MAX_BYTES 52U

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

static bool is_track_char(unsigned char value)
{
    return isalnum(value) || (value == '_') || (value == '-');
}

static bool is_display_name_byte_safe(unsigned char value)
{
    /* JSON quotes/backslashes and controls are never retained. UTF-8 bytes
     * are preserved verbatim as a display-only name; playback still uses the
     * independently generated ASCII logical ID. */
    return ((value >= 0x20U) && (value != '"') && (value != '\\'));
}

static bool extension_is_wav(const char *filename, size_t length)
{
    return (length >= 4U) &&
        (tolower((unsigned char)filename[length - 4U]) == '.') &&
        (tolower((unsigned char)filename[length - 3U]) == 'w') &&
        (tolower((unsigned char)filename[length - 2U]) == 'a') &&
        (tolower((unsigned char)filename[length - 1U]) == 'v');
}

static uint32_t filename_hash(const char *filename, size_t length)
{
    uint32_t value = 2166136261UL;
    for (size_t i = 0U; i < length; ++i) {
        value ^= (uint8_t)filename[i];
        value *= 16777619UL;
    }
    return value;
}

static bool make_entry(const char *filename, smart_room_catalog_entry_t *entry)
{
    if ((filename == NULL) || (entry == NULL)) return false;
    const size_t length = strnlen(filename, SMART_ROOM_AUDIO_FILENAME_MAX_BYTES);
    if ((length < 6U) || (length >= SMART_ROOM_AUDIO_FILENAME_MAX_BYTES) ||
        !extension_is_wav(filename, length)) return false;
    const size_t stem_length = length - 4U;
    if (stem_length >= XIAOZHI_FOUNDATION_AUDIO_TRACK_NAME_MAX_BYTES) return false;
    bool stem_is_token = true;
    for (size_t i = 0U; i < stem_length; ++i) {
        const unsigned char byte = (unsigned char)filename[i];
        if (!is_display_name_byte_safe(byte) || (byte == '/') || (byte == '\\') ||
            (byte == '.')) return false;
        if (!is_track_char(byte)) stem_is_token = false;
    }
    *entry = (smart_room_catalog_entry_t){0};
    if (stem_is_token) {
        memcpy(entry->public_track.id, filename, stem_length);
    } else {
        const int id_length = snprintf(entry->public_track.id,
                                       sizeof(entry->public_track.id),
                                       "track_%08lx",
                                       (unsigned long)filename_hash(filename, length));
        if ((id_length < 0) || ((size_t)id_length >= sizeof(entry->public_track.id)))
            return false;
    }
    memcpy(entry->public_track.name, filename, stem_length);
    memcpy(entry->filename, filename, length + 1U);
    return true;
}

static void insert_entry(smart_room_catalog_t *catalog,
                         const smart_room_catalog_entry_t *entry)
{
    uint8_t index = 0U;
    while ((index < catalog->count) &&
           (strcmp(catalog->entries[index].public_track.id,
                   entry->public_track.id) < 0)) ++index;
    if ((index < catalog->count) &&
        (strcmp(catalog->entries[index].public_track.id,
                entry->public_track.id) == 0)) return;
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
        if (sd_card_manager_is_vfs_media_error(open_error))
            sd_card_manager_report_io_error(ESP_FAIL);
        sd_card_manager_release();
        return ESP_FAIL;
    }
    catalog->available = true;
    struct dirent *node = NULL;
    while ((node = readdir(directory)) != NULL) {
        smart_room_catalog_entry_t entry = {0};
        if (!make_entry(node->d_name, &entry)) continue;
        char path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
        const int path_length = snprintf(path, sizeof(path), "%s/%s",
                                         SMART_ROOM_AUDIO_ROOT, entry.filename);
        if ((path_length < 0) || ((size_t)path_length >= sizeof(path))) continue;
        struct stat info = {0};
        if ((stat(path, &info) != 0) || !S_ISREG(info.st_mode) ||
            (info.st_size < 0)) continue;
        entry.public_track.size_bytes = (uint64_t)info.st_size;
        insert_entry(catalog, &entry);
    }
    const int close_result = closedir(directory);
    sd_card_manager_release();
    if (close_result != 0) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t copy_track_list(xiaozhi_foundation_audio_track_list_t *tracks,
                                 void *context)
{
    (void)context;
    if (tracks == NULL) return ESP_ERR_INVALID_ARG;
    smart_room_catalog_t catalog = {0};
    const esp_err_t ret = scan_catalog(&catalog);
    *tracks = (xiaozhi_foundation_audio_track_list_t){0};
    if (ret != ESP_OK) return ret;
    tracks->available = catalog.available;
    tracks->truncated = catalog.truncated;
    tracks->track_count = catalog.count;
    for (uint8_t i = 0U; i < catalog.count; ++i)
        tracks->tracks[i] = catalog.entries[i].public_track;
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
    const size_t id_length = strnlen(track_id, XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES);
    if ((id_length == 0U) || (id_length >= XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES) ||
        (strstr(track_id, "..") != NULL) || (strchr(track_id, '/') != NULL) ||
        (strchr(track_id, '\\') != NULL) || (strchr(track_id, '.') != NULL)) {
        result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_INVALID_REQUEST;
        return ESP_OK;
    }
    smart_room_catalog_t catalog = {0};
    const esp_err_t scan_ret = scan_catalog(&catalog);
    if (scan_ret != ESP_OK) {
        result->outcome = sd_card_manager_is_mounted()
            ? XIAOZHI_FOUNDATION_AUDIO_TRACK_CATALOG_UNAVAILABLE
            : XIAOZHI_FOUNDATION_AUDIO_TRACK_STORAGE_UNAVAILABLE;
        return ESP_OK;
    }
    for (uint8_t i = 0U; i < catalog.count; ++i) {
        if (strcmp(track_id, catalog.entries[i].public_track.id) != 0) continue;
        char path[SD_CARD_MANAGER_PATH_MAX_LEN] = {0};
        const int length = snprintf(path, sizeof(path), "%s/%s", SMART_ROOM_AUDIO_ROOT,
                                    catalog.entries[i].filename);
        if ((length < 0) || ((size_t)length >= sizeof(path))) break;
        const esp_err_t play_ret = voice_assistant_playback_start_catalog_wav(path);
        if (play_ret == ESP_OK) {
            result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS;
            result->accepted = true;
            result->scheduled = true;
        } else {
            result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_PLAYBACK_REJECTED;
        }
        return ESP_OK;
    }
    result->outcome = XIAOZHI_FOUNDATION_AUDIO_TRACK_NOT_FOUND;
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

esp_err_t smart_room_mcp_audio_tracks_register_providers(void)
{
    esp_err_t ret = xiaozhi_foundation_register_audio_track_list_provider(
        copy_track_list, NULL);
    if (ret != ESP_OK) return ret;
    ret = xiaozhi_foundation_register_audio_track_play_provider(play_track, NULL);
    if (ret != ESP_OK) return ret;
    return xiaozhi_foundation_register_audio_recorded_play_provider(
        play_recorded, NULL);
}
