#include <stdio.h>
#include <string.h>

#include "local_web_path_policy.h"
#include "local_web_audio_policy.h"
#include "local_web_download.h"
#include "local_web_icon_policy.h"
#include "local_web_light_policy.h"

static int expect_content_disposition(const char *path, const char *expected)
{
    char output[LOCAL_WEB_DOWNLOAD_CONTENT_DISPOSITION_MAX_LEN] = {0};
    if (!local_web_download_content_disposition(path, output, sizeof(output)) ||
        (strcmp(output, expected) != 0))
    {
        fprintf(stderr, "unexpected Content-Disposition for %s\n", path);
        return 1;
    }
    return 0;
}

static int expect_ok(const char *input, const char *expected)
{
    char output[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if ((local_web_path_policy_normalize(input, output, sizeof(output)) != ESP_OK) ||
        (strcmp(output, expected) != 0))
    {
        fprintf(stderr, "expected normalized path %s\n", expected);
        return 1;
    }
    return 0;
}

static int expect_rejected(const char *input)
{
    char output[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 1U] = {0};
    if (local_web_path_policy_normalize(input, output, sizeof(output)) == ESP_OK)
    {
        fprintf(stderr, "unsafe path accepted\n");
        return 1;
    }
    return 0;
}

static int expect_audio_policy(void)
{
    int failures = 0;
    local_web_audio_action_t action = LOCAL_WEB_AUDIO_ACTION_PAUSE;
    uint32_t volume = 0U;
    uint64_t frame = 0U;
    failures += !local_web_audio_action_parse("pause", &action) ||
                (action != LOCAL_WEB_AUDIO_ACTION_PAUSE);
    failures += !local_web_audio_action_parse("resume", &action) ||
                (action != LOCAL_WEB_AUDIO_ACTION_RESUME);
    failures += local_web_audio_action_parse("play", &action);
    failures += !local_web_audio_volume_percent_parse("0", &volume) || (volume != 0U);
    failures += !local_web_audio_volume_percent_parse("100", &volume) || (volume != 100U);
    failures += local_web_audio_volume_percent_parse("101", &volume);
    failures += local_web_audio_volume_percent_parse("50x", &volume);
    failures += local_web_audio_volume_percent_parse("", &volume);
    failures += !local_web_audio_uint64_parse("4294967296", &frame) ||
                (frame != UINT64_C(4294967296));
    failures += local_web_audio_uint64_parse("12x", &frame);
    failures += local_web_audio_uint64_parse("-1", &frame);
    failures += local_web_audio_uint64_parse("", &frame);
    failures += local_web_audio_uint64_parse("18446744073709551616", &frame);
    return failures;
}

static int expect_icon_policy(void)
{
    return (strcmp(local_web_icon_logical_path("storage"),
                   "/web-icons/hard-drive.svg") != 0) ||
           (strcmp(local_web_icon_logical_path("playback"),
                   "/web-icons/music-2.svg") != 0) ||
           (local_web_icon_logical_path("../audio/Input_1.wav") != NULL) ||
           (local_web_icon_logical_path("") != NULL);
}

static int expect_light_policy(void)
{
    int failures = 0;
    local_web_light_update_t full = {0};
    failures += !local_web_light_update_set_field(&full, "power", "true");
    failures += !local_web_light_update_set_field(&full, "red", "255");
    failures += !local_web_light_update_set_field(&full, "green", "0");
    failures += !local_web_light_update_set_field(&full, "blue", "128");
    failures += !local_web_light_update_set_field(&full, "brightness", "100");
    failures += !local_web_light_update_set_field(&full, "effect", "solid");
    failures += !local_web_light_update_is_valid(&full);

    light_manager_state_t state = {
        .power_on = false, .red = 1U, .green = 2U, .blue = 3U,
        .brightness_percent = 0U, .effect = LIGHT_MANAGER_EFFECT_BLINK,
    };
    failures += !local_web_light_apply_update(&full, &state) ||
                !state.power_on || (state.red != 255U) || (state.green != 0U) ||
                (state.blue != 128U) || (state.brightness_percent != 100U) ||
                (state.effect != LIGHT_MANAGER_EFFECT_SOLID);

    local_web_light_update_t partial = {0};
    failures += !local_web_light_update_set_field(&partial, "brightness", "0") ||
                !local_web_light_apply_update(&partial, &state) ||
                (state.brightness_percent != 0U) || (state.red != 255U) ||
                (state.effect != LIGHT_MANAGER_EFFECT_SOLID);

    const char *const effects[] = {
        "solid", "blink", "breath", "pulse", "rainbow", "strobe",
        "heartbeat", "candle",
    };
    for (size_t index = 0U; index < sizeof(effects) / sizeof(effects[0]); ++index) {
        local_web_light_update_t effect_update = {0};
        light_manager_state_t black_off = {0};
        failures += !local_web_light_update_set_field(&effect_update, "effect", effects[index]) ||
                    !local_web_light_apply_update(&effect_update, &black_off) ||
                    !black_off.power_on || (black_off.red != 255U) ||
                    (black_off.green != 255U) || (black_off.blue != 255U) ||
                    (black_off.effect != (light_manager_effect_t)index);
    }

    local_web_light_update_t off = {0};
    failures += !local_web_light_update_set_field(&off, "power", "false");
    state.effect = LIGHT_MANAGER_EFFECT_PULSE;
    state.red = 7U; state.green = 8U; state.blue = 9U; state.brightness_percent = 60U;
    failures += !local_web_light_apply_update(&off, &state) || state.power_on ||
                (state.red != 7U) || (state.green != 8U) || (state.blue != 9U) ||
                (state.brightness_percent != 60U) ||
                (state.effect != LIGHT_MANAGER_EFFECT_PULSE);

    local_web_light_update_t invalid = {0};
    failures += local_web_light_update_set_field(&invalid, "brightness", "101");
    failures += local_web_light_update_set_field(&invalid, "red", "256");
    failures += local_web_light_update_set_field(&invalid, "effect", "unknown");
    failures += local_web_light_update_set_field(&invalid, "power", "TRUE");
    failures += local_web_light_update_set_field(&invalid, "other", "1");
    failures += local_web_light_update_set_field(&invalid, "red", "0") == false;
    failures += local_web_light_update_set_field(&invalid, "red", "255");

    local_web_light_update_t effect_off = {0};
    failures += !local_web_light_update_set_field(&effect_off, "effect", "blink") ||
                !local_web_light_update_set_field(&effect_off, "power", "false") ||
                local_web_light_update_is_valid(&effect_off);
    failures += local_web_light_manager_result_from_error(ESP_ERR_INVALID_STATE) !=
                LOCAL_WEB_LIGHT_MANAGER_RESULT_UNAVAILABLE;
    failures += local_web_light_manager_result_from_error(ESP_ERR_TIMEOUT) !=
                LOCAL_WEB_LIGHT_MANAGER_RESULT_BUSY;
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += expect_ok("/", "/");
    failures += expect_ok("audio", "/audio");
    failures += expect_ok("nested/child", "/nested/child");
    failures += expect_ok("/nested/child", "/nested/child");
    failures += expect_ok("%2Faudio%2Fsample.wav", "/audio/sample.wav");

    failures += expect_rejected("..");
    failures += expect_rejected("../file");
    failures += expect_rejected("foo/../bar");
    failures += expect_rejected("/foo//bar");
    failures += expect_rejected("/foo/");
    failures += expect_rejected("/foo/%2e%2e/bar");
    failures += expect_rejected("/foo/%2Fbar");
    failures += expect_rejected("/foo/%5cbar");
    failures += expect_rejected("/foo/%ZZ");
    failures += expect_rejected("/foo/%");
    failures += expect_rejected("/foo\\bar");

    char long_path[LOCAL_WEB_LOGICAL_PATH_MAX_LEN + 2U];
    memset(long_path, 'a', sizeof(long_path) - 1U);
    long_path[sizeof(long_path) - 1U] = '\0';
    failures += expect_rejected(long_path);

    char long_component[LOCAL_WEB_LOGICAL_COMPONENT_MAX_LEN + 2U];
    memset(long_component, 'a', sizeof(long_component) - 1U);
    long_component[sizeof(long_component) - 1U] = '\0';
    failures += expect_rejected(long_component);

    failures += expect_content_disposition(
        "/audio/input_long_3.wav",
        "attachment; filename=\"input_long_3.wav\"; "
        "filename*=UTF-8''input_long_3.wav");
    failures += expect_content_disposition(
        "/audio/report 1.wav",
        "attachment; filename=\"report_1.wav\"; "
        "filename*=UTF-8''report%201.wav");
    failures += local_web_download_content_disposition(
        "/", long_path, sizeof(long_path));
    failures += expect_audio_policy();
    failures += expect_icon_policy();
    failures += expect_light_policy();
    return failures == 0 ? 0 : 1;
}
