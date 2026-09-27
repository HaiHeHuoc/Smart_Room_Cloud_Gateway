#pragma once

#include "wake_word_manager.h"

typedef enum {
    WAKE_WORD_RUNTIME_TRIGGER_ENABLE = 0,
    WAKE_WORD_RUNTIME_TRIGGER_DISABLE,
    WAKE_WORD_RUNTIME_TRIGGER_WAKE,
    WAKE_WORD_RUNTIME_TRIGGER_SPEECH,
    WAKE_WORD_RUNTIME_TRIGGER_UTTERANCE_COMPLETE,
    WAKE_WORD_RUNTIME_TRIGGER_REARM,
    WAKE_WORD_RUNTIME_TRIGGER_REARMED,
    WAKE_WORD_RUNTIME_TRIGGER_SUPPRESS,
    WAKE_WORD_RUNTIME_TRIGGER_MUTE,
    WAKE_WORD_RUNTIME_TRIGGER_UNMUTE,
} wake_word_runtime_trigger_t;

/* Pure transition policy: timestamps and AFE ownership stay in the manager. */
wake_word_manager_state_t wake_word_runtime_next_state(
    wake_word_manager_state_t current,
    wake_word_runtime_trigger_t trigger);
