#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SD_CARD_MANAGER_STATE_UNINITIALIZED = 0,
    SD_CARD_MANAGER_STATE_INITIALIZING,
    SD_CARD_MANAGER_STATE_MOUNTING,
    SD_CARD_MANAGER_STATE_RETRY_WAIT,
    SD_CARD_MANAGER_STATE_READY,
    SD_CARD_MANAGER_STATE_RECOVERING,
    SD_CARD_MANAGER_STATE_UNAVAILABLE,
} sd_card_manager_state_t;

typedef struct {
    sd_card_manager_state_t state;
    uint32_t active_leases;
} sd_card_manager_status_t;
