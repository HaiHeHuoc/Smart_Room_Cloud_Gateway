#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_manager.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_ASSISTANT_PLAYBACK_ACTION_PAUSE = 0,
    VOICE_ASSISTANT_PLAYBACK_ACTION_RESUME,
    VOICE_ASSISTANT_PLAYBACK_ACTION_STOP,
    VOICE_ASSISTANT_PLAYBACK_ACTION_RESTART,
} voice_assistant_playback_action_t;

typedef enum {
    VOICE_ASSISTANT_PLAYBACK_OUTCOME_SUCCESS = 0,
    VOICE_ASSISTANT_PLAYBACK_OUTCOME_NO_CURRENT_SOURCE,
    VOICE_ASSISTANT_PLAYBACK_OUTCOME_INVALID_STATE,
    VOICE_ASSISTANT_PLAYBACK_OUTCOME_NON_RESUMABLE_SOURCE,
    VOICE_ASSISTANT_PLAYBACK_OUTCOME_STALE_GENERATION,
    VOICE_ASSISTANT_PLAYBACK_OUTCOME_CONTROL_FAILED,
} voice_assistant_playback_outcome_t;

typedef struct {
    voice_assistant_playback_outcome_t outcome;
    bool accepted;
    bool physically_applied;
    audio_manager_playback_status_t playback;
} voice_assistant_playback_control_result_t;

/** Initialize bounded voice-turn playback policy state. No task is created. */
esp_err_t voice_assistant_playback_control_init(void);

/**
 * Prepare one retained physical PTT press. Resumable local playback is paused
 * and observed PAUSED with a finite wait before this call returns ESP_OK.
 * Non-seekable Xiaozhi response cancellation remains downlink-owned.
 */
esp_err_t voice_assistant_playback_prepare_ptt(
    uint32_t ptt_generation,
    uint32_t session_generation);

/** Transfer a still-valid suspended source to a new press interrupting TTS. */
esp_err_t voice_assistant_playback_supersede_ptt(
    uint32_t new_ptt_generation,
    uint32_t session_generation);

/** Mark the point after which release is a real voice turn, not a fast tap. */
esp_err_t voice_assistant_playback_mark_turn_started(uint32_t ptt_generation);

/** Restore an unstarted fast-release transaction when it is still current. */
esp_err_t voice_assistant_playback_cancel_unstarted(uint32_t ptt_generation);

/** Apply one generation-bound post-response action exactly once. */
esp_err_t voice_assistant_playback_finish_turn(uint32_t ptt_generation);

/** Bounded MCP-facing action entry; hardware ownership remains audio_manager. */
esp_err_t voice_assistant_playback_control(
    voice_assistant_playback_action_t action,
    voice_assistant_playback_control_result_t *result);

/** Copy the exact owner status without side effects. */
esp_err_t voice_assistant_playback_get_status(
    audio_manager_playback_status_t *status);

/**
 * Queue a project-owned catalog WAV. During a PTT turn any temporarily
 * suspended source is explicitly stopped, so its auto-resume cannot race the
 * newly selected track. The path is copied by audio_manager and must already
 * be resolved from a bounded catalog; it is not model input.
 */
esp_err_t voice_assistant_playback_start_catalog_wav(
    const char *resolved_path);

/** Start retained processed recording after the active TTS turn if needed. */
esp_err_t voice_assistant_playback_start_recorded(void);

#ifdef __cplusplus
}
#endif
