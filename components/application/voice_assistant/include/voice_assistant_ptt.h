#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** The frontend that currently owns a pressed PTT intent. */
typedef enum {
    VOICE_ASSISTANT_PTT_SOURCE_NONE = 0,
    VOICE_ASSISTANT_PTT_SOURCE_GPIO,
    VOICE_ASSISTANT_PTT_SOURCE_WEB,
} voice_assistant_ptt_source_t;

/* The browser renews at 2 s. Eight seconds tolerates short HTTP/Wi-Fi jitter
 * while still bounding a disappeared Local Web client. */
#define VOICE_ASSISTANT_PTT_WEB_KEEPALIVE_INTERVAL_MS 2000U
#define VOICE_ASSISTANT_PTT_WEB_LEASE_TIMEOUT_MS       8000U

typedef enum {
    VOICE_ASSISTANT_PTT_UNINITIALIZED = 0,
    VOICE_ASSISTANT_PTT_IDLE,
    VOICE_ASSISTANT_PTT_ARMING_SESSION,
    VOICE_ASSISTANT_PTT_SUSPENDING_PLAYBACK,
    VOICE_ASSISTANT_PTT_AUTHORIZED,
    VOICE_ASSISTANT_PTT_RELEASED,
    VOICE_ASSISTANT_PTT_CANCEL_PENDING,
    VOICE_ASSISTANT_PTT_ERROR,
} voice_assistant_ptt_state_t;

typedef struct {
    voice_assistant_ptt_state_t state;
    /** Source holding the current pressed intent, or NONE when no turn is held. */
    voice_assistant_ptt_source_t source;
    uint32_t ptt_generation;
    uint32_t session_generation;
    /** Monotonic debounced GPIO press delivery time, or zero before this intent. */
    int64_t pressed_at_us;
    /** Monotonic authorization time, or zero until a real READY path authorizes capture. */
    int64_t authorized_at_us;
    bool pressed;
    bool capture_authorized;
    esp_err_t last_error;
} voice_assistant_ptt_status_t;

typedef void (*voice_assistant_ptt_status_callback_t)(
    const voice_assistant_ptt_status_t *status,
    void *user_context);

/** Initialize the bounded PTT policy queue/task state. Does not own GPIO. */
esp_err_t voice_assistant_ptt_init(void);

/** Start the PTT policy task and enter IDLE. */
esp_err_t voice_assistant_ptt_start(void);

/** Stop the PTT policy task cooperatively and release its bounded state. */
esp_err_t voice_assistant_ptt_stop_and_deinit(void);

/**
 * Queue an authorized-user press intent.
 *
 * If the production voice session is IDLE, the PTT policy asks
 * voice_assistant to begin one session and waits asynchronously for READY. If
 * it is already CONNECTING, the press is armed and waits for that same READY
 * evidence. From ERROR, one continuously held press is retained through
 * bounded recovery and starts a fresh session after IDLE. Capture authorization
 * becomes true only after real READY evidence exists. A press during an old
 * Xiaozhi response requests its bounded downlink-owned cancellation and keeps
 * the same physical press. That abort forces a fresh WebSocket transport
 * generation, so the press remains ARMING_SESSION until new READY evidence
 * confirms the old packet stream cannot cross into it. Resumable local
 * playback must first reach a safe, resource-released PAUSED state; a fast
 * release never authorizes capture.
 */
esp_err_t voice_assistant_ptt_press(void);

/**
 * Queue release intent. Authorization is revoked by the PTT task. A release
 * remains FIFO-deliverable behind its still-pending press, so a short physical
 * tap cannot leave capture authorized after the button is already released.
 * Phase 14-B consumes this policy to start/stop microphone capture.
 */
esp_err_t voice_assistant_ptt_release(void);

/**
 * Cancel the current PTT intent. If a transport start is still pending,
 * cancellation remains pending until the bounded start resolves, then revokes
 * capture authorization without closing the long-lived production session.
 */
esp_err_t voice_assistant_ptt_cancel(void);

/**
 * Reserve one Web-owned PTT intent and return its generation.
 *
 * This queues the same policy path used by GPIO. It never touches I2S,
 * audio_manager, or the Xiaozhi transport. At most one source may reserve a
 * turn; a concurrent GPIO or Web request returns ESP_ERR_INVALID_STATE.
 */
esp_err_t voice_assistant_ptt_web_start(
    uint32_t client_id,
    uint32_t *ptt_generation);

/** Renew the bounded Web PTT lease for the matching client and generation. */
esp_err_t voice_assistant_ptt_web_keepalive(
    uint32_t client_id,
    uint32_t ptt_generation);

/** Release only the matching Web PTT generation; stale commands are rejected. */
esp_err_t voice_assistant_ptt_web_stop(
    uint32_t client_id,
    uint32_t ptt_generation);

/** Register/remove one copied PTT-status observer. */
esp_err_t voice_assistant_ptt_register_status_callback(
    voice_assistant_ptt_status_callback_t callback,
    void *user_context);

/** Copy the current PTT policy status. */
esp_err_t voice_assistant_ptt_get_status(voice_assistant_ptt_status_t *status);

const char *voice_assistant_ptt_state_to_string(voice_assistant_ptt_state_t state);
const char *voice_assistant_ptt_source_to_string(voice_assistant_ptt_source_t source);

#ifdef __cplusplus
}
#endif
