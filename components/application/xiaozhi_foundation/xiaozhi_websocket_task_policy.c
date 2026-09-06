#include <stdbool.h>
#include <string.h>

#include "esp_websocket_client.h"

/* esp_xiaozhi 0.1.2 leaves esp_websocket_client_config_t.task_stack at zero,
 * which selects the provider's 4 KiB default. WebSocket callbacks synchronously
 * dispatch Xiaozhi protocol, semantic-text, and copied UI-model work, so the
 * default overflowed on target. Keep the task in internal RAM and reserve a
 * measured safety margin without altering the managed component. */
#define XIAOZHI_WEBSOCKET_TASK_STACK_BYTES (12U * 1024U)

extern esp_websocket_client_handle_t __real_esp_websocket_client_init(
    const esp_websocket_client_config_t *config);

static bool xiaozhi_websocket_config_is_xiaozhi(
    const esp_websocket_client_config_t *config)
{
    return (config != NULL) &&
           (config->headers != NULL) &&
           (strstr(config->headers, "Protocol-Version:") != NULL);
}

esp_websocket_client_handle_t __wrap_esp_websocket_client_init(
    const esp_websocket_client_config_t *config)
{
    if (!xiaozhi_websocket_config_is_xiaozhi(config)) {
        return __real_esp_websocket_client_init(config);
    }

    /* The provider copies configuration values during init; this stack-local
     * descriptor is valid for the complete call and leaves caller storage
     * unchanged. Preserve a future upstream value only when it is already at
     * least as large as the project-owned safety margin. */
    esp_websocket_client_config_t xiaozhi_config = *config;
    if (xiaozhi_config.task_stack <
        (int)XIAOZHI_WEBSOCKET_TASK_STACK_BYTES) {
        xiaozhi_config.task_stack =
            (int)XIAOZHI_WEBSOCKET_TASK_STACK_BYTES;
    }

    return __real_esp_websocket_client_init(&xiaozhi_config);
}
