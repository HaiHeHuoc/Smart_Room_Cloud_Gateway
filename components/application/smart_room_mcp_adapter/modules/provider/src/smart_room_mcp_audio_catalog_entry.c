#include "smart_room_mcp_audio_catalog_entry.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static size_t bounded_length(const char *value, size_t capacity)
{
    size_t length = 0U;
    while ((length < capacity) && (value[length] != '\0')) {
        ++length;
    }
    return length;
}

static bool is_track_char(unsigned char value)
{
    return isalnum(value) || (value == '_') || (value == '-');
}

static bool is_display_name_byte_safe(unsigned char value)
{
    return ((value >= 0x20U) && (value != '"') && (value != '\\'));
}

static size_t utf8_sequence_length(unsigned char first)
{
    if (first < 0x80U) {
        return 1U;
    }
    if ((first >= 0xC2U) && (first <= 0xDFU)) {
        return 2U;
    }
    if ((first >= 0xE0U) && (first <= 0xEFU)) {
        return 3U;
    }
    if ((first >= 0xF0U) && (first <= 0xF4U)) {
        return 4U;
    }
    return 0U;
}

static bool utf8_is_valid(const char *value, size_t length)
{
    if (value == NULL) {
        return false;
    }
    for (size_t index = 0U; index < length;) {
        const unsigned char first = (unsigned char)value[index];
        const size_t sequence_length = utf8_sequence_length(first);
        if ((sequence_length == 0U) || ((length - index) < sequence_length)) {
            return false;
        }
        if (sequence_length == 1U) {
            ++index;
            continue;
        }
        for (size_t offset = 1U; offset < sequence_length; ++offset) {
            if (((unsigned char)value[index + offset] & 0xC0U) != 0x80U) {
                return false;
            }
        }
        const unsigned char next = (unsigned char)value[index + 1U];
        if (((first == 0xE0U) && (next < 0xA0U)) ||
            ((first == 0xEDU) && (next > 0x9FU)) ||
            ((first == 0xF0U) && (next < 0x90U)) ||
            ((first == 0xF4U) && (next > 0x8FU))) {
            return false;
        }
        index += sequence_length;
    }
    return true;
}

static bool extension_is_wav(const char *filename, size_t length)
{
    return (length >= 4U) &&
        (tolower((unsigned char)filename[length - 4U]) == '.') &&
        (tolower((unsigned char)filename[length - 3U]) == 'w') &&
        (tolower((unsigned char)filename[length - 2U]) == 'a') &&
        (tolower((unsigned char)filename[length - 1U]) == 'v');
}

static uint64_t filename_hash(const char *filename, size_t length)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t index = 0U; index < length; ++index) {
        value ^= (uint8_t)filename[index];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static size_t utf8_prefix_length(const char *value, size_t length, size_t limit)
{
    size_t copied = 0U;
    while (copied < length) {
        const size_t sequence_length =
            utf8_sequence_length((unsigned char)value[copied]);
        if ((sequence_length == 0U) ||
            (sequence_length > (limit - copied))) {
            break;
        }
        copied += sequence_length;
    }
    return copied;
}

bool smart_room_mcp_audio_catalog_entry_prepare(
    const char *filename,
    char *track_id,
    size_t track_id_capacity,
    char *display_name,
    size_t display_name_capacity,
    char *retained_filename,
    size_t retained_filename_capacity)
{
    if ((filename == NULL) || (track_id == NULL) || (track_id_capacity == 0U) ||
        (display_name == NULL) || (display_name_capacity == 0U) ||
        (retained_filename == NULL) || (retained_filename_capacity == 0U)) {
        return false;
    }

    const size_t length = bounded_length(filename, retained_filename_capacity);
    if ((length < 6U) || (length >= retained_filename_capacity) ||
        !extension_is_wav(filename, length)) {
        return false;
    }
    const size_t stem_length = length - 4U;
    if ((stem_length == 0U) || !utf8_is_valid(filename, stem_length)) {
        return false;
    }

    bool stem_is_token = true;
    for (size_t index = 0U; index < stem_length; ++index) {
        const unsigned char byte = (unsigned char)filename[index];
        if (!is_display_name_byte_safe(byte) || (byte == '/') ||
            (byte == '\\') || (byte == '.')) {
            return false;
        }
        if (!is_track_char(byte)) {
            stem_is_token = false;
        }
    }

    memset(track_id, 0, track_id_capacity);
    memset(display_name, 0, display_name_capacity);
    memset(retained_filename, 0, retained_filename_capacity);
    if (stem_is_token && (stem_length < track_id_capacity)) {
        memcpy(track_id, filename, stem_length);
    } else {
        const int id_length = snprintf(track_id,
                                       track_id_capacity,
                                       "track_%016llx",
                                       (unsigned long long)filename_hash(
                                           filename, length));
        if ((id_length < 0) || ((size_t)id_length >= track_id_capacity)) {
            return false;
        }
    }

    const size_t display_length = utf8_prefix_length(
        filename, stem_length, display_name_capacity - 1U);
    if (display_length == 0U) {
        return false;
    }
    memcpy(display_name, filename, display_length);
    memcpy(retained_filename, filename, length + 1U);
    return true;
}

uint64_t smart_room_mcp_audio_catalog_fatfs_size_bytes(int32_t vfs_stat_size)
{
    return (uint64_t)(uint32_t)vfs_stat_size;
}
