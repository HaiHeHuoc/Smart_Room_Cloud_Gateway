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

typedef enum {
    XIAOZHI_FOUNDATION_SESSION_STOPPED = 0,
    XIAOZHI_FOUNDATION_SESSION_CONNECTING,
    XIAOZHI_FOUNDATION_SESSION_READY,
    XIAOZHI_FOUNDATION_SESSION_ERROR,
} xiaozhi_foundation_session_state_t;

typedef struct {
    xiaozhi_foundation_session_state_t state;
    uint32_t client_generation;
    bool active;
    esp_err_t last_error;
} xiaozhi_foundation_session_status_t;

typedef void (*xiaozhi_foundation_session_status_callback_t)(
    const xiaozhi_foundation_session_status_t *status,
    void *user_context);

esp_err_t xiaozhi_foundation_session_register_status_callback(
    xiaozhi_foundation_session_status_callback_t callback,
    void *user_context);

esp_err_t xiaozhi_foundation_session_start(uint32_t client_generation);
esp_err_t xiaozhi_foundation_session_stop(void);
esp_err_t xiaozhi_foundation_session_get_status(
    xiaozhi_foundation_session_status_t *status);
const char *xiaozhi_foundation_session_state_to_string(
    xiaozhi_foundation_session_state_t state);

/* Phase 14 production audio boundary -------------------------------------- */

#define XIAOZHI_FOUNDATION_UPLINK_SAMPLE_RATE_HZ 16000U
#define XIAOZHI_FOUNDATION_UPLINK_CHANNELS       1U
#define XIAOZHI_FOUNDATION_OPUS_FRAME_DURATION_MS 60U
#define XIAOZHI_FOUNDATION_OPUS_FRAME_SAMPLES    960U
#define XIAOZHI_FOUNDATION_OPUS_MAX_PACKET_BYTES 2048U

typedef struct {
    bool audio_channel_open;
    bool listening;
    uint32_t client_generation;
    uint64_t frames_sent;
    uint64_t samples_sent;
    uint64_t bytes_sent;
    esp_err_t last_error;
} xiaozhi_foundation_audio_uplink_status_t;

/** Open the shared production audio channel and enter MANUAL listening. */
esp_err_t xiaozhi_foundation_audio_uplink_start(uint32_t client_generation);

/** Send one complete Opus packet from a non-audio callback task. */
esp_err_t xiaozhi_foundation_audio_uplink_send_opus_packet(
    uint32_t client_generation,
    const uint8_t *packet,
    size_t packet_size);

/**
 * Stop MANUAL listening after PTT release but keep the audio channel open.
 * The open channel is retained so the server can deliver the response audio.
 * Phase 14-D closes it only after response completion/abort.
 */
esp_err_t xiaozhi_foundation_audio_uplink_stop(uint32_t client_generation);

/** Close the shared production audio channel after response completion. */
esp_err_t xiaozhi_foundation_audio_channel_close(uint32_t client_generation);

esp_err_t xiaozhi_foundation_audio_uplink_get_status(
    xiaozhi_foundation_audio_uplink_status_t *status);

/** Project-owned classification of production response events. */
typedef enum {
    XIAOZHI_FOUNDATION_RESPONSE_TTS_START = 0,
    XIAOZHI_FOUNDATION_RESPONSE_AUDIO,
    XIAOZHI_FOUNDATION_RESPONSE_TTS_STOP,
    XIAOZHI_FOUNDATION_RESPONSE_ERROR,
} xiaozhi_foundation_response_event_kind_t;

/**
 * Borrowed response event. `data` is valid only during the callback and is
 * non-NULL only for RESPONSE_AUDIO. One RESPONSE_AUDIO callback contains one
 * complete Opus packet; callers must preserve that packet boundary.
 */
typedef struct {
    xiaozhi_foundation_response_event_kind_t kind;
    uint32_t client_generation;
    const uint8_t *data;
    size_t data_len;
    esp_err_t error;
} xiaozhi_foundation_response_event_t;

typedef void (*xiaozhi_foundation_response_callback_t)(
    const xiaozhi_foundation_response_event_t *event,
    void *user_context);

/** Register/remove the production response observer. Passing NULL removes it. */
esp_err_t xiaozhi_foundation_response_register_callback(
    xiaozhi_foundation_response_callback_t callback,
    void *user_context);

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
