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

/**
 * @brief Replace the active WebSocket transport without recreating the chat
 *        object or MCP engine.
 *
 * This is the hard packet fence for a locally aborted response. It stops the
 * old WebSocket task, drains its already-posted global events, then starts a
 * fresh transport under @p replacement_client_generation. Response delivery
 * remains disabled until the new connection is confirmed. It must run only
 * from the normal internal-RAM voice lifecycle task, never from a Xiaozhi
 * callback, ISR, or PSRAM-stack task. On failure the session enters ERROR and
 * remains fail-closed; callers must not authorize a new capture attempt.
 */
esp_err_t xiaozhi_foundation_session_rotate_transport(
    uint32_t expected_client_generation,
    uint32_t replacement_client_generation);

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

/* Phase 18.4 controlled cloud-push MCP boundary -------------------------- */

/** Bounded outcome returned by the project-owned cloud push provider. */
typedef enum {
    XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_ACCEPTED = 0,
    XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_NOT_READY,
    XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_OFFLINE,
    XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_BUSY,
    XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_INVALID_STATE,
    XIAOZHI_FOUNDATION_CLOUD_PUSH_LATEST_FAILED,
} xiaozhi_foundation_cloud_push_latest_outcome_t;

/** Copied result of asking the product cloud owner to push current telemetry. */
typedef struct {
    xiaozhi_foundation_cloud_push_latest_outcome_t outcome;
    /** True only when the manager accepted scheduling; never upload success. */
    bool accepted;
} xiaozhi_foundation_cloud_push_latest_result_t;

/**
 * @brief Request one bounded upload of the product-owned latest snapshot.
 *
 * The MCP callback calls this provider in normal task context. It must return
 * quickly, must not perform network I/O or LVGL work, and must not expose or
 * accept Firebase/auth/HTTP handles, credentials, endpoint paths, or payload
 * values. Registration is before voice start; the function/context remain
 * borrowed for firmware lifetime.
 */
typedef esp_err_t (*xiaozhi_foundation_cloud_push_latest_provider_t)(
    xiaozhi_foundation_cloud_push_latest_result_t *result,
    void *user_context);

/** Register the composition-owned bounded cloud push provider before voice start. */
esp_err_t xiaozhi_foundation_register_cloud_push_latest_provider(
    xiaozhi_foundation_cloud_push_latest_provider_t provider,
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
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_STROBE,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_HEARTBEAT,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_CANDLE,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_SOS,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_LIGHTNING,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_WAKE_UP,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_SLEEP_FADE,
    XIAOZHI_FOUNDATION_LIGHT_EFFECT_NOTIFICATION,
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

/* Phase 18.2.1 bounded audio playback MCP boundary ----------------------- */

typedef enum {
    XIAOZHI_FOUNDATION_AUDIO_ACTION_PAUSE = 0,
    XIAOZHI_FOUNDATION_AUDIO_ACTION_RESUME,
    XIAOZHI_FOUNDATION_AUDIO_ACTION_STOP,
    XIAOZHI_FOUNDATION_AUDIO_ACTION_RESTART,
} xiaozhi_foundation_audio_action_t;

typedef enum {
    XIAOZHI_FOUNDATION_AUDIO_STATE_IDLE = 0,
    XIAOZHI_FOUNDATION_AUDIO_STATE_STARTING,
    XIAOZHI_FOUNDATION_AUDIO_STATE_PLAYING,
    XIAOZHI_FOUNDATION_AUDIO_STATE_PAUSING,
    XIAOZHI_FOUNDATION_AUDIO_STATE_PAUSED,
    XIAOZHI_FOUNDATION_AUDIO_STATE_RESUMING,
    XIAOZHI_FOUNDATION_AUDIO_STATE_STOPPING,
    XIAOZHI_FOUNDATION_AUDIO_STATE_ERROR,
} xiaozhi_foundation_audio_state_t;

typedef enum {
    XIAOZHI_FOUNDATION_AUDIO_SOURCE_NONE = 0,
    XIAOZHI_FOUNDATION_AUDIO_SOURCE_RECORDED,
    XIAOZHI_FOUNDATION_AUDIO_SOURCE_WAV,
    XIAOZHI_FOUNDATION_AUDIO_SOURCE_LIVE_PCM,
} xiaozhi_foundation_audio_source_t;

typedef enum {
    XIAOZHI_FOUNDATION_AUDIO_PAUSE_NONE = 0,
    XIAOZHI_FOUNDATION_AUDIO_PAUSE_USER,
    XIAOZHI_FOUNDATION_AUDIO_PAUSE_PTT_TEMPORARY,
} xiaozhi_foundation_audio_pause_reason_t;

typedef enum {
    XIAOZHI_FOUNDATION_AUDIO_OUTCOME_SUCCESS = 0,
    XIAOZHI_FOUNDATION_AUDIO_OUTCOME_NO_CURRENT_SOURCE,
    XIAOZHI_FOUNDATION_AUDIO_OUTCOME_INVALID_STATE,
    XIAOZHI_FOUNDATION_AUDIO_OUTCOME_NON_RESUMABLE_SOURCE,
    XIAOZHI_FOUNDATION_AUDIO_OUTCOME_STALE_GENERATION,
    XIAOZHI_FOUNDATION_AUDIO_OUTCOME_CONTROL_FAILED,
} xiaozhi_foundation_audio_outcome_t;

/** Safe copied playback state; it contains no path, handle, pointer or PCM. */
typedef struct {
    bool available;
    xiaozhi_foundation_audio_state_t state;
    xiaozhi_foundation_audio_source_t source_type;
    xiaozhi_foundation_audio_pause_reason_t pause_reason;
    bool resumable;
    uint32_t generation;
    uint64_t position_frames;
    uint64_t total_frames;
    uint32_t position_granularity_frames;
} xiaozhi_foundation_audio_playback_snapshot_t;

typedef struct {
    xiaozhi_foundation_audio_outcome_t outcome;
    bool accepted;
    bool physically_applied;
    xiaozhi_foundation_audio_playback_snapshot_t playback;
} xiaozhi_foundation_audio_control_result_t;

typedef esp_err_t (*xiaozhi_foundation_audio_control_provider_t)(
    xiaozhi_foundation_audio_action_t action,
    xiaozhi_foundation_audio_control_result_t *result,
    void *user_context);

typedef esp_err_t (*xiaozhi_foundation_audio_state_provider_t)(
    xiaozhi_foundation_audio_playback_snapshot_t *snapshot,
    void *user_context);

/** Register the bounded control provider before production voice starts. */
esp_err_t xiaozhi_foundation_register_audio_control_provider(
    xiaozhi_foundation_audio_control_provider_t provider,
    void *user_context);

/** Register the side-effect-free copied playback-state provider. */
esp_err_t xiaozhi_foundation_register_audio_state_provider(
    xiaozhi_foundation_audio_state_provider_t provider,
    void *user_context);

/* Phase 18.2.2 bounded SD catalog MCP boundary -------------------------- */

#define XIAOZHI_FOUNDATION_AUDIO_TRACK_MAX_COUNT 12U
#define XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES 48U
#define XIAOZHI_FOUNDATION_AUDIO_TRACK_NAME_MAX_BYTES 48U

typedef struct {
    char id[XIAOZHI_FOUNDATION_AUDIO_TRACK_ID_MAX_BYTES];
    char name[XIAOZHI_FOUNDATION_AUDIO_TRACK_NAME_MAX_BYTES];
    /** File size copied from the catalog stat() result; no file handle escapes. */
    uint64_t size_bytes;
} xiaozhi_foundation_audio_track_t;

typedef struct {
    bool available;
    bool truncated;
    uint8_t track_count;
    xiaozhi_foundation_audio_track_t tracks[
        XIAOZHI_FOUNDATION_AUDIO_TRACK_MAX_COUNT];
} xiaozhi_foundation_audio_track_list_t;

typedef enum {
    XIAOZHI_FOUNDATION_AUDIO_TRACK_SUCCESS = 0,
    XIAOZHI_FOUNDATION_AUDIO_TRACK_INVALID_REQUEST,
    XIAOZHI_FOUNDATION_AUDIO_TRACK_NOT_FOUND,
    XIAOZHI_FOUNDATION_AUDIO_TRACK_STORAGE_UNAVAILABLE,
    XIAOZHI_FOUNDATION_AUDIO_TRACK_CATALOG_UNAVAILABLE,
    XIAOZHI_FOUNDATION_AUDIO_TRACK_PLAYBACK_REJECTED,
    XIAOZHI_FOUNDATION_AUDIO_TRACK_RECORDED_AUDIO_NOT_AVAILABLE,
    XIAOZHI_FOUNDATION_AUDIO_TRACK_INTERNAL_ERROR,
} xiaozhi_foundation_audio_track_outcome_t;

typedef struct {
    xiaozhi_foundation_audio_track_outcome_t outcome;
    bool accepted;
    bool scheduled;
} xiaozhi_foundation_audio_track_play_result_t;

typedef esp_err_t (*xiaozhi_foundation_audio_track_list_provider_t)(
    xiaozhi_foundation_audio_track_list_t *tracks,
    void *user_context);

typedef esp_err_t (*xiaozhi_foundation_audio_track_play_provider_t)(
    const char *track_id,
    xiaozhi_foundation_audio_track_play_result_t *result,
    void *user_context);
typedef esp_err_t (*xiaozhi_foundation_audio_recorded_play_provider_t)(
    xiaozhi_foundation_audio_track_play_result_t *result,
    void *user_context);

/** Register catalog providers before production voice starts. */
esp_err_t xiaozhi_foundation_register_audio_track_list_provider(
    xiaozhi_foundation_audio_track_list_provider_t provider,
    void *user_context);
esp_err_t xiaozhi_foundation_register_audio_track_play_provider(
    xiaozhi_foundation_audio_track_play_provider_t provider,
    void *user_context);
esp_err_t xiaozhi_foundation_register_audio_recorded_play_provider(
    xiaozhi_foundation_audio_recorded_play_provider_t provider,
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
 * This cleanup form keeps response delivery closed. Use it when no bounded
 * downlink response wait owns the retained channel.
 */
esp_err_t xiaozhi_foundation_audio_uplink_stop(uint32_t client_generation);

/**
 * @brief Stop MANUAL listening for an already-reserved current response.
 *
 * The caller must first reserve the matching downlink response epoch, then
 * call this from normal task context. The response-delivery gate opens just
 * before the stop-listening transmit so the first TTS/Opus callback cannot
 * race the synchronous send return. If that transmit fails, the caller must
 * cancel its response wait and close the channel when it still owns it; normal
 * channel cleanup closes the gate. This API never authorizes a future turn or
 * a response after a local abort/transport fence.
 */
esp_err_t xiaozhi_foundation_audio_uplink_stop_for_response(
    uint32_t client_generation);

/** Close the shared production audio channel after response completion. */
esp_err_t xiaozhi_foundation_audio_channel_close(uint32_t client_generation);

/**
 * @brief Best-effort request for the server to stop the current response.
 *
 * Call from normal task context before closing an interrupted response
 * channel. This sends the protocol abort while the server session ID is
 * still valid, then locally blocks response delivery. It is not the packet
 * identity boundary by itself; use session_rotate_transport() before a later
 * capture is authorized.
 */
esp_err_t xiaozhi_foundation_audio_abort_response(
    uint32_t client_generation);

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
