#include <stdio.h>

#include "wake_word_runtime_policy.h"

static unsigned s_failures;

#define EXPECT_EQ(actual, expected)                                             \
    do {                                                                         \
        if ((actual) != (expected)) {                                            \
            ++s_failures;                                                        \
            (void)fprintf(stderr, "FAIL %s:%d: %d != %d\n", __FILE__, __LINE__, \
                          (int)(actual), (int)(expected));                       \
        }                                                                        \
    } while (0)

int main(void)
{
    wake_word_manager_state_t state = WAKE_WORD_STATE_DISABLED;

    /* Wake, first speech, brief natural pause, and terminal utterance. */
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_ENABLE);
    EXPECT_EQ(state, WAKE_WORD_STATE_ARMED);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_WAKE);
    EXPECT_EQ(state, WAKE_WORD_STATE_WAIT_FOR_SPEECH);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_SPEECH);
    EXPECT_EQ(state, WAKE_WORD_STATE_LISTENING);
    EXPECT_EQ(wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_WAKE),
              WAKE_WORD_STATE_LISTENING);
    state = wake_word_runtime_next_state(state,
                                         WAKE_WORD_RUNTIME_TRIGGER_UTTERANCE_COMPLETE);
    EXPECT_EQ(state, WAKE_WORD_STATE_UTTERANCE_COMPLETE);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_REARM);
    EXPECT_EQ(state, WAKE_WORD_STATE_REARM_GUARD);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_REARMED);
    EXPECT_EQ(state, WAKE_WORD_STATE_ARMED);

    /* No-speech, playback/PTT suppression, mute, and disabled state are terminal. */
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_WAKE);
    EXPECT_EQ(state, WAKE_WORD_STATE_WAIT_FOR_SPEECH);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_REARM);
    EXPECT_EQ(state, WAKE_WORD_STATE_REARM_GUARD);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_REARMED);
    EXPECT_EQ(state, WAKE_WORD_STATE_ARMED);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_SUPPRESS);
    EXPECT_EQ(state, WAKE_WORD_STATE_SUPPRESSED);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_REARM);
    EXPECT_EQ(state, WAKE_WORD_STATE_REARM_GUARD);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_MUTE);
    EXPECT_EQ(state, WAKE_WORD_STATE_DISABLED);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_UNMUTE);
    EXPECT_EQ(state, WAKE_WORD_STATE_REARM_GUARD);
    state = wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_MUTE);
    EXPECT_EQ(state, WAKE_WORD_STATE_DISABLED);
    EXPECT_EQ(wake_word_runtime_next_state(state, WAKE_WORD_RUNTIME_TRIGGER_WAKE),
              WAKE_WORD_STATE_DISABLED);

    if (s_failures != 0U) {
        (void)fprintf(stderr, "%u wake runtime policy test(s) failed\n", s_failures);
        return 1;
    }
    (void)printf("wake runtime policy: PASS\n");
    return 0;
}
