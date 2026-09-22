#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "smart_room_mcp_audio_catalog_entry.h"

#define TRACK_FIELD_BYTES 48U
#define FILENAME_BYTES 65U

static int utf8_is_valid(const char *value)
{
    const unsigned char *bytes = (const unsigned char *)value;
    for (size_t index = 0U; bytes[index] != '\0';) {
        const unsigned char first = bytes[index];
        size_t count = 0U;
        if (first < 0x80U) {
            count = 1U;
        } else if ((first >= 0xC2U) && (first <= 0xDFU)) {
            count = 2U;
        } else if ((first >= 0xE0U) && (first <= 0xEFU)) {
            count = 3U;
        } else if ((first >= 0xF0U) && (first <= 0xF4U)) {
            count = 4U;
        } else {
            return 0;
        }
        for (size_t offset = 1U; offset < count; ++offset) {
            if ((bytes[index + offset] & 0xC0U) != 0x80U) {
                return 0;
            }
        }
        index += count;
    }
    return 1;
}

static int prepare(const char *filename,
                   char track_id[TRACK_FIELD_BYTES],
                   char display_name[TRACK_FIELD_BYTES],
                   char retained_filename[FILENAME_BYTES])
{
    return smart_room_mcp_audio_catalog_entry_prepare(
        filename,
        track_id,
        TRACK_FIELD_BYTES,
        display_name,
        TRACK_FIELD_BYTES,
        retained_filename,
        FILENAME_BYTES) ? 0 : 1;
}

int main(void)
{
    int failures = 0;
    char id[TRACK_FIELD_BYTES] = {0};
    char name[TRACK_FIELD_BYTES] = {0};
    char retained[FILENAME_BYTES] = {0};
    int checkpoint = failures;

    failures += prepare("input_long_3.wav", id, name, retained);
    failures += strcmp(id, "input_long_3") != 0;
    failures += strcmp(name, "input_long_3") != 0;
    failures += strcmp(retained, "input_long_3.wav") != 0;
    if (failures != checkpoint) {
        fprintf(stderr, "input_long_3 catalog case failed\n");
    }

    memset(id, 0, sizeof(id));
    memset(name, 0, sizeof(name));
    memset(retained, 0, sizeof(retained));
    const char *const vietnamese =
        "Ki\xE1\xBA\xBFp Ch\xE1\xBB\x93ng Chung - B\xC3\xB9i C\xC3\xB4ng Nam - "
        "Ma OST - Official MV.wav";
    checkpoint = failures;
    failures += prepare(vietnamese, id, name, retained);
    failures += strncmp(id, "track_", 6U) != 0;
    failures += strlen(id) != 22U;
    failures += (strlen(name) > (TRACK_FIELD_BYTES - 1U)) || !utf8_is_valid(name);
    failures += strcmp(retained, vietnamese) != 0;
    if (failures != checkpoint) {
        fprintf(stderr, "UTF-8 catalog case failed: id=%s name=%s retained=%s\n",
                id, name, retained);
    }

    char longest_filename[FILENAME_BYTES] = {0};
    memset(longest_filename, 'a', 60U);
    memcpy(&longest_filename[60], ".wav", 5U);
    memset(id, 0, sizeof(id));
    memset(name, 0, sizeof(name));
    memset(retained, 0, sizeof(retained));
    checkpoint = failures;
    failures += prepare(longest_filename, id, name, retained);
    failures += strncmp(id, "track_", 6U) != 0;
    failures += strlen(name) != (TRACK_FIELD_BYTES - 1U);
    failures += strcmp(retained, longest_filename) != 0;
    if (failures != checkpoint) {
        fprintf(stderr, "maximum filename catalog case failed\n");
    }

    char too_long[FILENAME_BYTES + 1U] = {0};
    memset(too_long, 'a', 61U);
    memcpy(&too_long[61], ".wav", 5U);
    checkpoint = failures;
    failures += smart_room_mcp_audio_catalog_entry_prepare(
        too_long, id, sizeof(id), name, sizeof(name), retained, sizeof(retained));
    if (failures != checkpoint) {
        fprintf(stderr, "overlong filename rejection case failed\n");
    }

    checkpoint = failures;
    failures += smart_room_mcp_audio_catalog_fatfs_size_bytes(2147483647) !=
                2147483647ULL;
    failures += smart_room_mcp_audio_catalog_fatfs_size_bytes(
                    (int32_t)0x80000000U) != 2147483648ULL;
    failures += smart_room_mcp_audio_catalog_fatfs_size_bytes(
                    (int32_t)0xFFFFFFFFU) != 4294967295ULL;
    if (failures != checkpoint) {
        fprintf(stderr, "FATFS size conversion case failed\n");
    }

    if (failures != 0) {
        fprintf(stderr, "smart_room_mcp_audio_catalog_entry: %d failures\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
