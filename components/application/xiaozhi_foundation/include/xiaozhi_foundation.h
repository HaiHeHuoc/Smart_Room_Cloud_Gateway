#pragma once

/**
 * @file xiaozhi_foundation.h
 * @brief Project-owned Xiaozhi boundary for service, session, audio, and validation use.
 *
 * Public structures contain copied scalar/project-owned state only. Xiaozhi
 * handles, transport endpoints, credentials, tokens, and callback-lifetime
 * framework pointers never cross this boundary.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Caller-owned scalar snapshot of non-sensitive Xiaozhi service state. */
typedef struct {
    bool service_reachable;
    bool websocket_available;
    bool activation_code_available;
    bool activation_challenge_available;
    int activation_timeout_ms;
    bool server_time_available;
    bool new_firmware_available;
} xiaozhi_foundation_info_t;

/** Project transport selection. MQTT is intentionally unsupported. */
typedef enum {
    XIAOZHI_FOUNDATION_TRANSPORT_AUTO = 0,
    XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET,
} xiaozhi_foundation_transport_t;

/* Phase 13 production session boundary ------------------------------------ */

/** Observable state of the project-owned long-lived Xiaozhi chat session. */
typedef enum {
    XIAOZHI_FOUNDATION_SESSION_STOPPED = 0,
    XIAOZHI_FOUNDATION_SESSION_CONNECTING,
    XIAOZHI_FOUNDATION_SESSION_READY,
    XIAOZHI_FOUNDATION_SESSION_ERROR,
} xiaozhi_foundation_session_state_t;

/**
 * @brief Copied session state emitted by the production-facing foundation API.
 *
 * client_generation is an opaque application-supplied generation value. The
 * foundation copies it into every callback so an orchestrator can reject late
 * events from an older conversation without exposing Xiaozhi internals.
 */
typedef struct {
    xiaozhi_foundation_session_state_t state;
    uint32_t client_generation;
    bool active;
    esp_err_t last_error;
} xiaozhi_foundation_session_status_t;

/**
 * @brief Receive one borrowed copied session snapshot.
 *
 * The callback runs in normal task/event-loop context and is never called while
 * the foundation session lock is held. Keep it short and copy/queue the status
 * before returning. Do not call LVGL or perform blocking lifecycle work here.
 */
typedef void (*xiaozhi_foundation_session_status_callback_t)(
    const xiaozhi_foundation_session_status_t *status,
    void *user_context);

esp_err_t xiaozhi_foundation_session_register_status_callback(
    xiaozhi_foundation_session_status_callback_t callback,
    void *user_context);

/** Start one long-lived WebSocket Xiaozhi chat session. */
esp_err_t xiaozhi_foundation_session_start(uint32_t client_generation);

/** Stop and release the active production Xiaozhi chat session. */
esp_err_t xiaozhi_foundation_session_stop(void);

/** Copy the current production session status. */
esp_err_t xiaozhi_foundation_session_get_status(
    xiaozhi_foundation_session_status_t *status);

/** Convert one production session state to a stable diagnostic string. */
const char *xiaozhi_foundation_session_state_to_string(
    xiaozhi_foundation_session_state_t state);

/* Phase 14 production audio-uplink boundary ------------------------------- */

#define XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ 16000U
#define XIAOZHI_FOUNDATION_UPLINK_CHANNELS       1U
#define XIAOZHI_FOUNDATION_UPLINK_FRAME_SAMPLES  256U

/** Copied scalar diagnostics for one production microphone uplink. */
typedef struct {
    bool audio_channel_open;
    bool listening;
    uint32_t client_generation;
    uint64_t frames_sent;
    uint64_t samples_sent;
    uint64_t bytes_sent;
    esp_err_t last_error;
} xiaozhi_foundation_audio_uplink_status_t;

/**
 * @brief Open the production audio channel and enter MANUAL listening mode.
 *
 * The parent production session must already be READY for the same non-zero
 * client generation. The Phase-14 MVP advertises PCM, 16 kHz, mono, 16 ms
 * frames (256 PCM16 samples). No I2S or audio-manager ownership is taken here.
 */
esp_err_t xiaozhi_foundation_audio_uplink_start(uint32_t client_generation);

/**
 * @brief Send one PCM16 microphone frame through the active Xiaozhi channel.
 *
 * This may perform transport work and therefore must never be called from the
 * audio-manager frame callback. The caller must use its own bounded queue/task.
 */
esp_err_t xiaozhi_foundation_audio_uplink_send_pcm16(
    uint32_t client_generation,
    const int16_t *samples,
    size_t sample_count);

/**
 * @brief End MANUAL listening and close the production audio channel.
 *
 * The long-lived Phase-13 chat session remains alive/READY after a successful
 * uplink stop so a later PTT turn may reuse it.
 */
esp_err_t xiaozhi_foundation_audio_uplink_stop(uint32_t client_generation);

/** Copy current audio-uplink scalar diagnostics. */
esp_err_t xiaozhi_foundation_audio_uplink_get_status(
    xiaozhi_foundation_audio_uplink_status_t *status);

/* Temporary Phase 12 validation UI status -------------------------------- */

#define XIAOZHI_FOUNDATION_UI_TEXT_BUFFER_SIZE 192U

typedef enum {
    XIAOZHI_FOUNDATION_UI_DISCONNECTED = 0,
    XIAOZHI_FOUNDATION_UI_READY,
    XIAOZHI_FOUNDATION_UI_LISTENING,
    XIAOZHI_FOUNDATION_UI_PROCESSING,
    XIAOZHI_FOUNDATION_UI_RESPONDING,
    XIAOZHI_FOUNDATION_UI_ERROR,
} xiaozhi_foundation_ui_state_t;

typedef struct {
    xiaozhi_foundation_ui_state_t state;
    int64_t listening_started_at_us;
    int64_t listening_stopped_at_us;
    esp_err_t last_error;
    bool user_text_truncated;
    char user_text[XIAOZHI_FOUNDATION_UI_TEXT_BUFFER_SIZE];
    bool assistant_text_truncated;
    char assistant_text[XIAOZHI_FOUNDATION_UI_TEXT_BUFFER_SIZE];
} xiaozhi_foundation_ui_status_t;

typedef void (*xiaozhi_foundation_ui_status_callback_t)(
    const xiaozhi_foundation_ui_status_t *status,
    void *user_context);

/* Existing Phase 12 service/validation API -------------------------------- */

esp_err_t xiaozhi_foundation_probe(xiaozhi_foundation_info_t *out_info);
esp_err_t xiaozhi_foundation_request_probe(void);
esp_err_t xiaozhi_foundation_request_transport_validation(
    xiaozhi_foundation_transport_t transport);
esp_err_t xiaozhi_foundation_register_ui_status_callback(
    xiaozhi_foundation_ui_status_callback_t callback,
    void *user_context);

#ifdef __cplusplus
}
#endif
