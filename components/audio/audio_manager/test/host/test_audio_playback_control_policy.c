#include <stdbool.h>
#include <stdio.h>

#include "audio_manager_playback_control_policy.h"

static bool expect_transition(
    audio_manager_playback_control_state_t current,
    audio_manager_playback_control_event_t event,
    esp_err_t expected_result,
    audio_manager_playback_control_state_t expected_next)
{
    audio_manager_playback_control_state_t next = current;
    const esp_err_t result = audio_manager_playback_control_transition(
        current,
        event,
        &next);
    return (result == expected_result) &&
           ((result != ESP_OK) || (next == expected_next));
}

int main(void)
{
    bool ok = true;
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE,
                            AUDIO_MANAGER_PLAYBACK_EVENT_BEGIN,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_PLAY_APPLIED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_APPLIED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED,
                            AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED,
                            AUDIO_MANAGER_PLAYBACK_EVENT_RESUME_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_PLAY_APPLIED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_RESTART_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_STARTING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED,
                            AUDIO_MANAGER_PLAYBACK_EVENT_RESTART_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_RESUMING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_STOP_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_STOPPING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSED,
                            AUDIO_MANAGER_PLAYBACK_EVENT_STOP_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_STOPPING);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE,
                            AUDIO_MANAGER_PLAYBACK_EVENT_STOP_REQUESTED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE,
                            AUDIO_MANAGER_PLAYBACK_EVENT_RESUME_REQUESTED,
                            ESP_ERR_INVALID_STATE,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE,
                            AUDIO_MANAGER_PLAYBACK_EVENT_PAUSE_REQUESTED,
                            ESP_ERR_INVALID_STATE,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE);
    /* Natural EOF may win while a cooperative pause is still being applied. */
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PAUSING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_FINISHED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_PLAYING,
                            AUDIO_MANAGER_PLAYBACK_EVENT_FAILED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR);
    ok &= expect_transition(AUDIO_MANAGER_PLAYBACK_CONTROL_ERROR,
                            AUDIO_MANAGER_PLAYBACK_EVENT_RECOVERED,
                            ESP_OK,
                            AUDIO_MANAGER_PLAYBACK_CONTROL_IDLE);

    printf("[%s] playback control transition matrix\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
