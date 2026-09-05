#include "esp_websocket_client.h"

#include "freertos/FreeRTOS.h"

/* Keep every production WebSocket write bounded. esp_xiaozhi 0.1.2 passes
 * portMAX_DELAY to both public send APIs; if a half-open socket stops draining,
 * that can otherwise freeze the caller before its application recovery logic
 * can run. Eight seconds bounds the affected send operation while the existing
 * session-close and WebSocket reconnect policies remain independent. */
#define XIAOZHI_WEBSOCKET_TX_TIMEOUT_MS 8000U

extern int __real_esp_websocket_client_send_text(
    esp_websocket_client_handle_t client,
    const char *data,
    int len,
    TickType_t timeout);

extern int __real_esp_websocket_client_send_bin(
    esp_websocket_client_handle_t client,
    const char *data,
    int len,
    TickType_t timeout);

static TickType_t xiaozhi_websocket_tx_timeout(TickType_t requested_timeout)
{
    return (requested_timeout == portMAX_DELAY) ?
               pdMS_TO_TICKS(XIAOZHI_WEBSOCKET_TX_TIMEOUT_MS) :
               requested_timeout;
}

int __wrap_esp_websocket_client_send_text(
    esp_websocket_client_handle_t client,
    const char *data,
    int len,
    TickType_t timeout)
{
    return __real_esp_websocket_client_send_text(
        client, data, len, xiaozhi_websocket_tx_timeout(timeout));
}

int __wrap_esp_websocket_client_send_bin(
    esp_websocket_client_handle_t client,
    const char *data,
    int len,
    TickType_t timeout)
{
    return __real_esp_websocket_client_send_bin(
        client, data, len, xiaozhi_websocket_tx_timeout(timeout));
}
