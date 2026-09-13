#include <stdbool.h>
#include <stdio.h>

#include "xiaozhi_mcp_audio_playback_policy.h"

static bool expect_action(
    const char *text,
    xiaozhi_foundation_audio_action_t expected)
{
    xiaozhi_foundation_audio_action_t action = {0};
    return xiaozhi_mcp_audio_playback_parse_action(text, &action) &&
           (action == expected);
}

int main(void)
{
    bool ok = true;
    ok &= expect_action("pause", XIAOZHI_FOUNDATION_AUDIO_ACTION_PAUSE);
    ok &= expect_action("resume", XIAOZHI_FOUNDATION_AUDIO_ACTION_RESUME);
    ok &= expect_action("stop", XIAOZHI_FOUNDATION_AUDIO_ACTION_STOP);
    ok &= expect_action("restart", XIAOZHI_FOUNDATION_AUDIO_ACTION_RESTART);

    xiaozhi_foundation_audio_action_t action = {0};
    ok &= !xiaozhi_mcp_audio_playback_parse_action(NULL, &action);
    ok &= !xiaozhi_mcp_audio_playback_parse_action("", &action);
    ok &= !xiaozhi_mcp_audio_playback_parse_action("play", &action);
    ok &= !xiaozhi_mcp_audio_playback_parse_action("Pause", &action);
    ok &= !xiaozhi_mcp_audio_playback_parse_action("resume_now", &action);
    ok &= !xiaozhi_mcp_audio_playback_parse_action("stop", NULL);

    puts(ok ? "MCP audio action policy: PASS" :
              "MCP audio action policy: FAIL");
    return ok ? 0 : 1;
}
