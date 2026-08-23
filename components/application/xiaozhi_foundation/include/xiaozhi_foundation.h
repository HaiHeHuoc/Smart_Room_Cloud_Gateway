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

    bool mqtt_available;
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
 * This selection contains no credentials, endpoint values, or other
 * sensitive transport configuration.
 */
typedef enum {
    XIAOZHI_FOUNDATION_TRANSPORT_AUTO = 0,
    XIAOZHI_FOUNDATION_TRANSPORT_MQTT,
    XIAOZHI_FOUNDATION_TRANSPORT_WEBSOCKET,
} xiaozhi_foundation_transport_t;

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
 * @brief Request validation of a selected Xiaozhi transport.
 *
 * @param[in] transport Transport mode to validate.
 *
 * This interface is intended for normal task context and is not ISR-safe.
 * Its blocking, timeout, result, and ownership contract will be defined with
 * the transport-validation implementation.
 */
esp_err_t xiaozhi_foundation_request_transport_validation(
    xiaozhi_foundation_transport_t transport);

#ifdef __cplusplus
}
#endif
