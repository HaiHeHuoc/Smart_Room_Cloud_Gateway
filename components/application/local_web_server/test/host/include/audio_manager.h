#pragma once

typedef enum {
    AUDIO_MANAGER_STATE_UNINITIALIZED = 0,
    AUDIO_MANAGER_STATE_INITIALIZED,
    AUDIO_MANAGER_STATE_IDLE,
    AUDIO_MANAGER_STATE_RECORDING,
    AUDIO_MANAGER_STATE_PROCESSING,
    AUDIO_MANAGER_STATE_PLAYBACK,
    AUDIO_MANAGER_STATE_ERROR,
} audio_manager_state_t;

typedef struct {
    audio_manager_state_t state;
} audio_manager_status_t;
