#pragma once

#include <stdbool.h>

typedef enum {
    TIME_MANAGER_STATE_UNINITIALIZED = 0,
    TIME_MANAGER_STATE_INITIALIZED,
    TIME_MANAGER_STATE_WAITING_NETWORK,
    TIME_MANAGER_STATE_SYNCING,
    TIME_MANAGER_STATE_SYNCED,
    TIME_MANAGER_STATE_RETRY_WAIT,
    TIME_MANAGER_STATE_ERROR,
} time_manager_state_t;

typedef struct {
    time_manager_state_t state;
    bool synced;
} time_manager_status_t;
