#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Submit a bounded notification WAV request through the playback arbiter. */
esp_err_t audio_manager_play_notification_wav(
    uint32_t request_id,
    const char *path);

/** Submit a critical alarm WAV request eligible to preempt lower-priority audio. */
esp_err_t audio_manager_play_critical_alarm_wav(
    uint32_t request_id,
    const char *path);

/**
 * Submit one already-validated catalog WAV through the normal bounded arbiter.
 *
 * `path` is copied and must have been constructed by a project-owned catalog
 * resolver. This is intentionally not an MCP/path parsing API: remote callers
 * supply only a logical track id and never call this function directly.
 */
esp_err_t audio_manager_play_catalog_wav(
    uint32_t request_id,
    const char *path);

#ifdef __cplusplus
}
#endif
