#include <stdbool.h>
#include <stdio.h>

#include "voice_recording_critical.h"

static unsigned s_enter_events = 0U;
static unsigned s_exit_events = 0U;

static void observe_transition(
    bool active,
    uint32_t transition_sequence,
    void *context)
{
    (void)transition_sequence;
    (void)context;
    if (active) {
        ++s_enter_events;
    } else {
        ++s_exit_events;
    }
}

static bool expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return false;
    }
    return true;
}

int main(void)
{
    bool ok = true;
    voice_recording_critical_status_t status = {0};

    ok &= expect(!voice_recording_critical_is_active(), "starts inactive");
    ok &= expect(voice_recording_critical_enter(0U, 1U) == ESP_ERR_INVALID_ARG,
                 "zero session rejected");
    ok &= expect(voice_recording_critical_exit(1U, 0U) == ESP_ERR_INVALID_ARG,
                 "zero PTT rejected");
    ok &= expect(voice_recording_critical_register_listener(
                     observe_transition, NULL) == ESP_OK,
                 "listener registers");
    ok &= expect(voice_recording_critical_enter(7U, 11U) == ESP_OK,
                 "enters after a real capture owner is supplied");
    ok &= expect(voice_recording_critical_is_active(),
                 "background policy observes active window");
    voice_recording_critical_get_status(&status);
    ok &= expect(status.active && status.session_generation == 7U &&
                     status.ptt_generation == 11U,
                 "owner snapshot is coherent");
    ok &= expect(voice_recording_critical_enter(7U, 11U) == ESP_OK,
                 "same owner entry is idempotent");
    ok &= expect(voice_recording_critical_exit(8U, 11U) == ESP_ERR_INVALID_STATE,
                 "stale cleanup cannot clear a newer owner");
    ok &= expect(voice_recording_critical_exit(7U, 11U) == ESP_OK,
                 "normal release exits window");
    ok &= expect(!voice_recording_critical_is_active(),
                 "background policy resumes after release");
    ok &= expect(voice_recording_critical_exit(7U, 11U) == ESP_OK,
                 "repeated cleanup is idempotent");

    for (uint32_t turn = 1U; turn <= 55U; ++turn) {
        ok &= expect(voice_recording_critical_enter(100U + turn, turn) == ESP_OK,
                     "repeated turn enters");
        ok &= expect(voice_recording_critical_exit(100U + turn, turn) == ESP_OK,
                     "repeated turn exits");
    }

    voice_recording_critical_get_status(&status);
    ok &= expect(!status.active, "55 repeated turns never latch active");
    ok &= expect(status.entered_count == 56U && status.exited_count == 56U,
                 "all transitions are accounted");
    ok &= expect(s_enter_events == 56U && s_exit_events == 56U,
                 "listeners see every completed transition");

    if (ok) {
        puts("voice_recording_critical tests: PASS");
    }
    return ok ? 0 : 1;
}
