#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Prepare one bounded public catalog entry from a direct WAV filename.
 *
 * The complete filename is retained for the trusted internal playback path.
 * The public display name is UTF-8-safe truncated to its supplied capacity;
 * non-token or overlong token stems receive a deterministic ASCII hash ID.
 */
bool smart_room_mcp_audio_catalog_entry_prepare(
    const char *filename,
    char *track_id,
    size_t track_id_capacity,
    char *display_name,
    size_t display_name_capacity,
    char *retained_filename,
    size_t retained_filename_capacity);

/** Recover the unsigned FATFS byte count represented by a signed VFS size. */
uint64_t smart_room_mcp_audio_catalog_fatfs_size_bytes(int32_t vfs_stat_size);
