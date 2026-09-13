#pragma once

/**
 * @file xiaozhi_foundation.h
 * @brief Project-owned Xiaozhi boundary for production session, audio, and MCP use.
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

/* Production Smart Room sensor MCP boundary ------------------------------ */

/** A copied, non-sensitive sample supplied to the read-only MCP tool. */
typedef struct {
    bool available;
    float temperature_c;
    float humidity_percent;
} xiaozhi_foundation_sensor_query_snapshot_t;

/**
 * @brief Copy the latest valid, non-stale room sensor sample.
 *
 * The callback runs in normal task context for a no-argument MCP tool call.
 * It must not perform I/O, state changes, LVGL calls, or expose identifiers,
 * credentials, logs, or provider handles.
 */
typedef esp_err_t (*xiaozhi_foundation_sensor_query_provider_t)(
    xiaozhi_foundation_sensor_query_snapshot_t *snapshot,
    void *user_context);

/**
 * @brief Register the composition-owned room sensor snapshot provider.
 *
 * Registration must happen before the production voice session starts. The
 * provider and context remain borrowed for the firmware lifetime.
 */
esp_err_t xiaozhi_foundation_register_sensor_query_provider(
    xiaozhi_foundation_sensor_query_provider_t provider,
    void *user_context);

/* Production Smart Room cloud-sync MCP boundary -------------------------- */

#define XIAOZHI_FOUNDATION_CLOUD_SYNC_STATE_MAX_BYTES          24U
#define XIAOZHI_FOUNDATION_CLOUD_SYNC_FAILURE_CLASS_MAX_BYTES  32U

/** A copied, non-sensitive cloud uploader snapshot for the read-only MCP tool. */
typedef struct {
    bool available;
    char state[XIAOZHI_FOUNDATION_CLOUD_SYNC_STATE_MAX_BYTES];
    bool last_success_available;
    uint32_t last_success_age_seconds;
    uint32_t consecutive_failure_count;
    char failure_class[XIAOZHI_FOUNDATION_CLOUD_SYNC_FAILURE_CLASS_MAX_BYTES];
    bool retry_scheduled;
    uint32_t retry_delay_seconds;
} xiaozhi_foundation_cloud_sync_query_snapshot_t;

/**
 * @brief Copy the current non-sensitive cloud uploader status.
 *
 * The callback runs in normal task context for a no-argument MCP tool call.
 * It must not perform I/O, state changes, LVGL calls, or expose endpoint
 * URLs, HTTP bodies, credentials, tokens, identifiers, logs, or provider
 * handles.
 */
typedef esp_err_t (*xiaozhi_foundation_cloud_sync_query_provider_t)(
    xiaozhi_foundation_cloud_sync_query_snapshot_t *snapshot,
    void *user_context);

/**
 * @brief Register the composition-owned cloud-sync snapshot provider.
 *
 * Registration must happen before the production voice session starts. The
 * provider and context remain borrowed for the firmware lifetime.
 */
esp_err_t xiaozhi_foundation_register_cloud_sync_query_provider(
    xiaozhi_foundation_cloud_sync_query_provider_t provider,
    void *user_context);

/* Production Smart Room system-status MCP boundary ----------------------- */

#define XIAOZHI_FOUNDATION_SYSTEM_STATUS_TOKEN_MAX_BYTES  16U

/** A copied, normalized local-health snapshot for the read-only MCP tool. */
typedef struct {
    bool available;
    char overall_state[XIAOZHI_FOUNDATION_SYSTEM_STATUS_TOKEN_MAX_BYTES];
    char sensor_state[XIAOZHI_FOUNDATION_SYSTEM_STATUS_TOKEN_MAX_BYTES];
    char cloud_state[XIAOZHI_FOUNDATION_SYSTEM_STATUS_TOKEN_MAX_BYTES];
    char time_state[XIAOZHI_FOUNDATION_SYSTEM_STATUS_TOKEN_MAX_BYTES];
    char storage_state[XIAOZHI_FOUNDATION_SYSTEM_STATUS_TOKEN_MAX_BYTES];
    char audio_state[XIAOZHI_FOUNDATION_SYSTEM_STATUS_TOKEN_MAX_BYTES];
} xiaozhi_foundation_system_status_query_snapshot_t;

/**
 * @brief Copy normalized, non-sensitive local component health states.
 *
 * The callback runs in normal task context for a no-argument MCP tool call.
 * It must not perform I/O, state changes, LVGL calls, or expose Wi-Fi details,
 * network reachability, endpoints, credentials, tokens, identifiers, logs,
 * files, or provider handles. The call itself cannot prove Internet
 * reachability because Xiaozhi must already have a live session to invoke it.
 */
typedef esp_err_t (*xiaozhi_foundation_system_status_query_provider_t)(
    xiaozhi_foundation_system_status_query_snapshot_t *snapshot,
    void *user_context);

/**
 * @brief Register the composition-owned system-status snapshot provider.
 *
 * Registration must happen before the production voice session starts. The
 * provider and context remain borrowed for the firmware lifetime.
 */
esp_err_t xiaozhi_foundation_register_system_status_query_provider(
    xiaozhi_foundation_system_status_query_provider_t provider,
    void *user_context);

/* Production Smart Room light-control MCP boundary ----------------------- */

/** Bounded product effect values shared by the light MCP providers. */
typedef enum {
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_SOLID = 0,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_BLINK,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_BREATH,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_PULSE,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_RAINBOW,
} xiaozhi_foundation_light_effect_t;

/** A validated, bounded logical light partial-update request. */
typedef struct {
    bool has_power;
    bool power_on;
    bool has_color;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    bool has_brightness;
    uint8_t brightness_percent;
    bool has_effect;
    xiaozhi_foundation_light_effect_t effect;
} xiaozhi_foundation_light_set_state_request_t;

typedef enum {
    XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SUCCESS = 0,
    XIAOZHI_FOUNDATION_LIGHT_SET_STATE_MANAGER_NOT_INITIALIZED,
    XIAOZHI_FOUNDATION_LIGHT_SET_STATE_SNAPSHOT_FAILED,
    XIAOZHI_FOUNDATION_LIGHT_SET_STATE_APPLY_FAILED,
} xiaozhi_foundation_light_set_state_outcome_t;

/** A copied product result returned by the composition-owned light adapter. */
typedef struct {
    xiaozhi_foundation_light_set_state_outcome_t outcome;
    bool power_on;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness_percent;
    xiaozhi_foundation_light_effect_t effect;
} xiaozhi_foundation_light_set_state_result_t;

/**
 * @brief Apply one fully validated logical light request in normal task context.
 *
 * The provider is borrowed for firmware lifetime and must route only through
 * the product light owner. It must not call GPIO, NeoPixel, RMT, LVGL, or
 * provider APIs directly. On a successful manager call, it copies the final
 * manager state into @p result; failures use a bounded outcome classification.
 */
typedef esp_err_t (*xiaozhi_foundation_light_set_state_provider_t)(
    const xiaozhi_foundation_light_set_state_request_t *request,
    xiaozhi_foundation_light_set_state_result_t *result,
    void *user_context);

/** Register the composition-owned controlled light provider before voice start. */
esp_err_t xiaozhi_foundation_register_light_set_state_provider(
    xiaozhi_foundation_light_set_state_provider_t provider,
    void *user_context);

/* Production Smart Room light-state MCP boundary ------------------------- */

/** A copied logical light state supplied to the read-only MCP tool. */
typedef struct {
    bool available;
    bool power_on;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness_percent;
    xiaozhi_foundation_light_effect_t effect;
} xiaozhi_foundation_light_state_query_snapshot_t;

/**
 * @brief Copy the current logical light state in normal task context.
 *
 * The provider is borrowed for firmware lifetime and must route only through
 * the product light owner. It must not access GPIO, NeoPixel, RMT, LVGL, or
 * provider APIs directly, and it must not make any state change.
 */
typedef esp_err_t (*xiaozhi_foundation_light_state_query_provider_t)(
    xiaozhi_foundation_light_state_query_snapshot_t *snapshot,
    void *user_context);

/** Register the composition-owned read-only light provider before voice start. */
esp_err_t xiaozhi_foundation_register_light_state_query_provider(
    xiaozhi_foundation_light_state_query_provider_t provider,
    void *user_context);

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

/* Phase 15 production semantic text boundary ------------------------------ */

typedef enum {
    XIAOZHI_FOUNDATION_TEXT_ROLE_USER = 0,
    XIAOZHI_FOUNDATION_TEXT_ROLE_ASSISTANT,
} xiaozhi_foundation_text_role_t;

/**
 * Borrowed semantic text event promoted from Xiaozhi CHAT_TEXT.
 *
 * `text` is valid only during the callback. Consumers must copy it before
 * returning. `client_generation` identifies the long-lived production voice
 * session that produced the event.
 */
typedef struct {
    xiaozhi_foundation_text_role_t role;
    uint32_t client_generation;
    const char *text;
} xiaozhi_foundation_text_event_t;

typedef void (*xiaozhi_foundation_text_callback_t)(
    const xiaozhi_foundation_text_event_t *event,
    void *user_context);

/** Register/remove the single production semantic-text observer. */
esp_err_t xiaozhi_foundation_text_register_callback(
    xiaozhi_foundation_text_callback_t callback,
    void *user_context);

#ifdef __cplusplus
}
#endif
