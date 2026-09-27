#include "wake_word_runtime_policy.h"

wake_word_manager_state_t wake_word_runtime_next_state(
    wake_word_manager_state_t current,
    wake_word_runtime_trigger_t trigger)
{
    switch (trigger) {
        case WAKE_WORD_RUNTIME_TRIGGER_ENABLE:
            return WAKE_WORD_STATE_ARMED;
        case WAKE_WORD_RUNTIME_TRIGGER_DISABLE:
        case WAKE_WORD_RUNTIME_TRIGGER_MUTE:
            return WAKE_WORD_STATE_DISABLED;
        case WAKE_WORD_RUNTIME_TRIGGER_WAKE:
            return (current == WAKE_WORD_STATE_ARMED)
                ? WAKE_WORD_STATE_WAIT_FOR_SPEECH : current;
        case WAKE_WORD_RUNTIME_TRIGGER_SPEECH:
            return (current == WAKE_WORD_STATE_WAIT_FOR_SPEECH)
                ? WAKE_WORD_STATE_LISTENING : current;
        case WAKE_WORD_RUNTIME_TRIGGER_UTTERANCE_COMPLETE:
            return (current == WAKE_WORD_STATE_LISTENING)
                ? WAKE_WORD_STATE_UTTERANCE_COMPLETE : current;
        case WAKE_WORD_RUNTIME_TRIGGER_REARM:
            return ((current == WAKE_WORD_STATE_DISABLED) ||
                    (current == WAKE_WORD_STATE_ERROR))
                ? current : WAKE_WORD_STATE_REARM_GUARD;
        case WAKE_WORD_RUNTIME_TRIGGER_REARMED:
            return (current == WAKE_WORD_STATE_REARM_GUARD)
                ? WAKE_WORD_STATE_ARMED : current;
        case WAKE_WORD_RUNTIME_TRIGGER_SUPPRESS:
            return ((current == WAKE_WORD_STATE_DISABLED) ||
                    (current == WAKE_WORD_STATE_ERROR))
                ? current : WAKE_WORD_STATE_SUPPRESSED;
        case WAKE_WORD_RUNTIME_TRIGGER_UNMUTE:
            return (current == WAKE_WORD_STATE_ERROR)
                ? current : WAKE_WORD_STATE_REARM_GUARD;
        default:
            return WAKE_WORD_STATE_ERROR;
    }
}
