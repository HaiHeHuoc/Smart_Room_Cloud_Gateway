#pragma once

#include <stdbool.h>

typedef enum {
    WIFI_MANAGER_STATE_UNINITIALIZED = 0,
    WIFI_MANAGER_STATE_READY,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_WAITING_FOR_IP,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_DISCONNECTED,
    WIFI_MANAGER_STATE_FAILED,
    WIFI_MANAGER_STATE_RETRY_WAIT,
} wifi_manager_state_t;

typedef struct {
    wifi_manager_state_t state;
    bool has_ipv4_address;
} wifi_manager_status_t;
