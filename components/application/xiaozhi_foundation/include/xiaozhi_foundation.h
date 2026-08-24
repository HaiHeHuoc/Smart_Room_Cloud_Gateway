#pragma once

/**
 * @file xiaozhi_foundation.h
 * @brief Public non-sensitive Xiaozhi service-information probe.
 *
 * The component returns caller-owned scalar service state only. It does not
 * expose Xiaozhi-owned pointers and does not start, stop, or own Wi-Fi or
 * Xiaozhi lifecycle management.
 */

/* Includes ----------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Type Definitions --------------------------------------------------------- */
/**
 * @brief Caller-owned scalar snapshot of non-sensitive Xiaozhi service state.
 *
 * Availability flags reveal only whether a service value is present. This
 * structure never contains activation data, tokens, endpoint strings, or
 * Xiaozhi-owned pointers.
 */
typedef struct {
    bool service_reachable;

    bool websocket_available;

    bool activation_code_available;
    bool activation_challenge_available;
    int activation_timeout_ms;

    bool server_time_available;
    bool new_firmware_available;
} xiaozhi_foundation_info_t;

/**
 * @brief Requested transport for a Xiaozhi transport-validation operation.
 *
 * The project has closed MQTT support for Xiaozhi. AUTO is retained only as a
 * compatibility request mode and resolves to WebSocket. This selection
 * contains no credentials, endpoint values, or other sensitive transport
 * configuration.
 */
typedef enum {
    XIAOZHI_FOUNDATION_TRANSPORT_AUTO = 0,
    XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET,
} xiaozhi_foundation_transport_t;

/* Temporary Phase 12.5 validation UI status ------------------------------ */

/** Maximum bytes, including the NUL terminator, retained for each transcript. */
#define XIAOZHI_FOUNDATION_UI_TEXT_BUFFER_SIZE 192U

/**
 * @brief Non-production interaction facts exposed by the Phase 12 validator.
 *
 * These states describe the temporary WebSocket validation worker only. They
 * are not a project voice-assistant lifecycle API and do not start, stop, or
 * otherwise own network or audio hardware.
 */
typedef enum {
    XIAOZHI_FOUNDATION_UI_DISCONNECTED = 0,
    XIAOZHI_FOUNDATION_UI_READY,
    XIAOZHI_FOUNDATION_UI_LISTENING,
    XIAOZHI_FOUNDATION_UI_PROCESSING,
    XIAOZHI_FOUNDATION_UI_RESPONDING,
    XIAOZHI_FOUNDATION_UI_ERROR,
} xiaozhi_foundation_ui_state_t;

/**
 * @brief Caller-owned, bounded snapshot for a temporary validation display.
 *
 * Text is copied from the borrowed Xiaozhi callback payload before this
 * structure is emitted. It contains no credentials, endpoint, token, raw
 * audio, framework-owned pointer, or raw protocol error-source string.
 */
typedef struct {
    xiaozhi_foundation_ui_state_t state;

    /** Monotonic timestamps from esp_timer_get_time(), or zero when absent. */
    int64_t listening_started_at_us;
    int64_t listening_stopped_at_us;

    /** ESP-IDF error code for ERROR; ESP_OK for non-error states. */
    esp_err_t last_error;

    bool user_text_truncated;
    char user_text[XIAOZHI_FOUNDATION_UI_TEXT_BUFFER_SIZE];

    bool assistant_text_truncated;
    char assistant_text[XIAOZHI_FOUNDATION_UI_TEXT_BUFFER_SIZE];
} xiaozhi_foundation_ui_status_t;

/**
 * @brief Receive one borrowed Phase 12.5 validation UI snapshot.
 *
 * The callback may run in the Xiaozhi validation worker or an ESP event-loop
 * task. It must return promptly, never call LVGL directly, and copy @p status
 * before returning if it needs to retain any field.
 */
typedef void (*xiaozhi_foundation_ui_status_callback_t)(
    const xiaozhi_foundation_ui_status_t *status,
    void *user_context);

/* Functions ---------------------------------------------------------------- */
/**
 * @brief Probe Xiaozhi and copy non-sensitive service state to caller storage.
 *
 * @param[out] out_info Writable destination for the scalar snapshot. It is
 *                       cleared before the service request. The caller owns
 *                       this storage and no release operation is required.
 *
 * @return ESP_OK when the service information is copied and released;
 *         ESP_ERR_INVALID_ARG when @p out_info is NULL;
 *         ESP_ERR_INVALID_STATE while another foundation operation is active;
 *         otherwise an error from the underlying Xiaozhi get-info or release
 *         operation.
 *
 * @pre Network connectivity must already be available. This function does not
 *      start Wi-Fi and does not own its lifecycle.
 *
 * Call from normal task context only; it is not ISR-safe. Blocking and timeout
 * behavior are determined by the underlying Xiaozhi API. This wrapper adds no
 * retry or timeout policy and exposes no Xiaozhi-owned pointer after return.
 */
esp_err_t xiaozhi_foundation_probe(xiaozhi_foundation_info_t *out_info);

/**
 * @brief Request a one-shot Xiaozhi service probe.
 *
 * This function is non-blocking. The HTTP probe runs in a worker task.
 *
 * @return
 *      - ESP_OK if the probe task was started
 *      - ESP_ERR_INVALID_STATE if a probe is already running
 *      - ESP_ERR_NO_MEM if the worker task could not be created
 */
esp_err_t xiaozhi_foundation_request_probe(void);

/**
 * @brief Request one temporary Phase 12 WebSocket validation checkpoint.
 *
 * @param[in] transport AUTO or WebSocket. AUTO resolves to WebSocket because
 *                       MQTT is intentionally not supported by the project.
 *
 * The default checkpoint is P2-E: connect, open a WebSocket audio channel,
 * observe the component event, hold briefly, close, stop, and deinitialize.
 * P2-F known-audio E2E and the Phase 12.6 repeated lifecycle matrix are each
 * selected only by their explicit validation-only Kconfig options. The matrix
 * owns a new context and chat handle for every cycle and stops after the first
 * failed cycle. No checkpoint is a production voice API or an audio-hardware
 * integration.
 *
 * This interface is intended for normal task context and is not ISR-safe.
 * Its blocking, timeout, result, and ownership contract are implemented by the
 * foundation worker task, which uses a normal internal-RAM task stack.
 */
esp_err_t xiaozhi_foundation_request_transport_validation(
    xiaozhi_foundation_transport_t transport);

/**
 * @brief Register the one temporary Phase 12.5 validation UI observer.
 *
 * The application composition layer uses this bridge to copy validation facts
 * into its GUI queue. Registration is accepted only while no foundation
 * operation is running. The component borrows both callback and context until
 * application shutdown; callers must keep them valid and must not call LVGL
 * from the callback.
 *
 * @param[in] callback Non-NULL observer that copies the supplied snapshot.
 * @param[in] user_context Caller-owned opaque context, optionally NULL.
 * @return ESP_OK on registration, ESP_ERR_INVALID_ARG for NULL callback, or
 *         ESP_ERR_INVALID_STATE while a probe or validation is active.
 */
esp_err_t xiaozhi_foundation_register_ui_status_callback(
    xiaozhi_foundation_ui_status_callback_t callback,
    void *user_context);

#ifdef __cplusplus
}
#endif
