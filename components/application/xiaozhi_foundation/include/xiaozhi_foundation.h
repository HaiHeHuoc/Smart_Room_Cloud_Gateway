#pragma once

/**
 * @file xiaozhi_foundation.h
 * @brief Project-owned Xiaozhi boundary for service, session, and validation use.
 *
 * Public structures contain copied scalar/project-owned state only. Xiaozhi
 * handles, transport endpoints, credentials, tokens, and callback-lifetime
 * framework pointers never cross this boundary.
 */

#include <stdbool.h>
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

/**
 * @brief Register or remove the production session observer.
 *
 * Passing NULL unregisters it. Registration is rejected while a production
 * session start/stop lifecycle is active or while a session is connected.
 */
esp_err_t xiaozhi_foundation_session_register_status_callback(
    xiaozhi_foundation_session_status_callback_t callback,
    void *user_context);

/**
 * @brief Start one long-lived WebSocket Xiaozhi chat session.
 *
 * This task-context API performs service-info retrieval, creates the project-
 * owned MCP/chat objects, starts WebSocket transport, and waits up to the
 * bounded connection timeout for a real CONNECTED event. On success the chat
 * remains alive in READY until xiaozhi_foundation_session_stop() is called or
 * an asynchronous transport failure is reported. No audio channel is opened
 * here and no microphone/speaker/I2S resource is owned.
 *
 * @param client_generation Opaque non-zero application generation copied into
 *        all session callbacks for stale-event rejection.
 */
esp_err_t xiaozhi_foundation_session_start(uint32_t client_generation);

/**
 * @brief Stop and release the active production Xiaozhi chat session.
 *
 * Cleanup order follows the validated Phase-12 lifecycle: chat stop, event
 * handler unregister, chat deinit, MCP destroy, then EventGroup release. The
 * call is task-context only and must be serialized by the application.
 */
esp_err_t xiaozhi_foundation_session_stop(void);

/** @brief Copy the current production session status. */
esp_err_t xiaozhi_foundation_session_get_status(
    xiaozhi_foundation_session_status_t *status);

/** Convert one production session state to a stable diagnostic string. */
const char *xiaozhi_foundation_session_state_to_string(
    xiaozhi_foundation_session_state_t state);

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

/** Probe Xiaozhi and copy non-sensitive service state to caller storage. */
esp_err_t xiaozhi_foundation_probe(xiaozhi_foundation_info_t *out_info);

/** Request a one-shot background service probe. */
esp_err_t xiaozhi_foundation_request_probe(void);

/** Request one temporary Phase-12 transport-validation checkpoint. */
esp_err_t xiaozhi_foundation_request_transport_validation(
    xiaozhi_foundation_transport_t transport);

/** Register the temporary Phase-12 validation UI observer. */
esp_err_t xiaozhi_foundation_register_ui_status_callback(
    xiaozhi_foundation_ui_status_callback_t callback,
    void *user_context);

#ifdef __cplusplus
}
#endif
