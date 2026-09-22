#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the bounded Smart Room MCP providers with Xiaozhi Foundation.
 *
 * This composition-owned call installs the existing sensor, cloud-sync,
 * system-status, logical-light-state, and controlled-light-state providers.
 * It must run after their manager owners are initialized and before the
 * production voice session is started. Provider callbacks are borrowed for
 * firmware lifetime and only copy public manager snapshots or invoke the
 * logical light-manager API; they never access Xiaozhi transport handles.
 *
 * @return ESP_OK when every provider was registered, otherwise the first
 *         registration error. Registration has no hardware side effects.
 */
esp_err_t smart_room_mcp_adapter_register_providers(void);

/* Shared bounded audio-catalog seam. It intentionally exposes copied logical
 * identities only: callers never receive paths, SD handles, or MCP internals. */
#define SMART_ROOM_AUDIO_CATALOG_MAX_TRACKS 12U
#define SMART_ROOM_AUDIO_CATALOG_TRACK_ID_MAX_BYTES 48U
#define SMART_ROOM_AUDIO_CATALOG_TRACK_NAME_MAX_BYTES 48U
#define SMART_ROOM_AUDIO_CATALOG_FILENAME_MAX_BYTES 65U

typedef struct {
    char id[SMART_ROOM_AUDIO_CATALOG_TRACK_ID_MAX_BYTES];
    char name[SMART_ROOM_AUDIO_CATALOG_TRACK_NAME_MAX_BYTES];
    /** Approved basename only; never an absolute filesystem path. */
    char filename[SMART_ROOM_AUDIO_CATALOG_FILENAME_MAX_BYTES];
    uint64_t size_bytes;
} smart_room_audio_catalog_track_t;

typedef struct {
    bool available;
    bool truncated;
    uint8_t track_count;
    smart_room_audio_catalog_track_t tracks[SMART_ROOM_AUDIO_CATALOG_MAX_TRACKS];
} smart_room_audio_catalog_t;

typedef enum {
    SMART_ROOM_AUDIO_CATALOG_PLAY_SUCCESS = 0,
    SMART_ROOM_AUDIO_CATALOG_PLAY_INVALID_REQUEST,
    SMART_ROOM_AUDIO_CATALOG_PLAY_STORAGE_UNAVAILABLE,
    SMART_ROOM_AUDIO_CATALOG_PLAY_UNAVAILABLE,
    SMART_ROOM_AUDIO_CATALOG_PLAY_NOT_FOUND,
    SMART_ROOM_AUDIO_CATALOG_PLAY_REJECTED,
    SMART_ROOM_AUDIO_CATALOG_PLAY_INTERNAL_ERROR,
} smart_room_audio_catalog_play_outcome_t;

typedef struct {
    smart_room_audio_catalog_play_outcome_t outcome;
    bool accepted;
    bool scheduled;
} smart_room_audio_catalog_play_result_t;

/** Copy the shared cached `/sdcard/audio` catalog without filesystem I/O. */
esp_err_t smart_room_mcp_adapter_audio_catalog_get(
    smart_room_audio_catalog_t *catalog);

/** Resolve one approved logical ID internally and submit it to voice policy. */
esp_err_t smart_room_mcp_adapter_audio_catalog_play(
    const char *track_id,
    smart_room_audio_catalog_play_result_t *result);

#ifdef __cplusplus
}
#endif
