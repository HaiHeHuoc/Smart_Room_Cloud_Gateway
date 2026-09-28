#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "esp_err.h"
#include "voice_assistant_bootstrap_lifecycle.h"

enum { STAGE_COUNT = 12 };

typedef struct {
    int fail_start_index;
    int fail_rollback_index;
    unsigned starts[STAGE_COUNT];
    unsigned rollbacks[STAGE_COUNT];
    int active[STAGE_COUNT];
} fake_runtime_t;

static esp_err_t fake_start(void *context, size_t index)
{
    fake_runtime_t *const runtime = context;
    assert(runtime->active[index] == 0); /* no duplicate worker/resource */
    ++runtime->starts[index];
    runtime->active[index] = 1;
    return ((int)index == runtime->fail_start_index) ? ESP_FAIL : ESP_OK;
}

static esp_err_t fake_rollback(void *context, size_t index)
{
    fake_runtime_t *const runtime = context;
    ++runtime->rollbacks[index];
    if ((int)index == runtime->fail_rollback_index) {
        return ESP_ERR_TIMEOUT; /* Owner may still access dependencies. */
    }
    runtime->active[index] = 0;
    return ESP_OK;
}

#define DECLARE_STAGE(index) \
    static esp_err_t start_##index(void *context) { return fake_start(context, index); } \
    static esp_err_t rollback_##index(void *context) { return fake_rollback(context, index); }

DECLARE_STAGE(0) DECLARE_STAGE(1) DECLARE_STAGE(2) DECLARE_STAGE(3)
DECLARE_STAGE(4) DECLARE_STAGE(5) DECLARE_STAGE(6) DECLARE_STAGE(7)
DECLARE_STAGE(8) DECLARE_STAGE(9) DECLARE_STAGE(10) DECLARE_STAGE(11)

static const voice_assistant_bootstrap_stage_t s_stages[STAGE_COUNT] = {
    { "audio", start_0, rollback_0 },
    { "playback_arbiter", start_1, rollback_1 },
    { "capture_arbiter", start_2, rollback_2 },
    { "voice_task", start_3, rollback_3 },
    { "ui_model", start_4, rollback_4 },
    { "ui_adapter", start_5, rollback_5 },
    { "playback_control", start_6, rollback_6 },
    { "ptt", start_7, rollback_7 },
    { "uplink", start_8, rollback_8 },
    { "downlink", start_9, rollback_9 },
    { "gpio38", start_10, rollback_10 },
    { "xiaozhi_session", start_11, rollback_11 },
};

static void test_failure_rolls_back_and_retries(size_t failed_stage)
{
    fake_runtime_t runtime = { .fail_start_index = (int)failed_stage,
                               .fail_rollback_index = -1 };
    voice_assistant_bootstrap_lifecycle_t lifecycle;
    voice_assistant_bootstrap_lifecycle_init(&lifecycle, s_stages, STAGE_COUNT);
    esp_err_t rollback_error = ESP_OK;
    assert(voice_assistant_bootstrap_lifecycle_start(&lifecycle, &runtime,
                                                      &rollback_error) == ESP_FAIL);
    assert(rollback_error == ESP_OK);
    assert(lifecycle.safe_for_retry);
    for (size_t i = 0; i < STAGE_COUNT; ++i) {
        assert(runtime.active[i] == 0);
        assert(runtime.rollbacks[i] == ((i <= failed_stage) ? 1U : 0U));
    }

    runtime.fail_start_index = -1;
    assert(voice_assistant_bootstrap_lifecycle_start(&lifecycle, &runtime,
                                                      &rollback_error) == ESP_OK);
    for (size_t i = 0; i < STAGE_COUNT; ++i) {
        assert(runtime.active[i] == 1);
    }
    assert(voice_assistant_bootstrap_lifecycle_rollback(&lifecycle, &runtime) == ESP_OK);
    assert(voice_assistant_bootstrap_lifecycle_rollback(&lifecycle, &runtime) == ESP_OK);
}

static void test_cleanup_failure_blocks_retry(void)
{
    fake_runtime_t runtime = { .fail_start_index = 10, .fail_rollback_index = 7 };
    voice_assistant_bootstrap_lifecycle_t lifecycle;
    voice_assistant_bootstrap_lifecycle_init(&lifecycle, s_stages, STAGE_COUNT);
    esp_err_t rollback_error = ESP_OK;
    assert(voice_assistant_bootstrap_lifecycle_start(&lifecycle, &runtime,
                                                      &rollback_error) == ESP_FAIL);
    assert(rollback_error == ESP_ERR_TIMEOUT);
    assert(!lifecycle.safe_for_retry);
    assert(lifecycle.attempted_count == 8U);
    for (size_t i = 0; i < 7U; ++i) {
        assert(runtime.rollbacks[i] == 0U);
        assert(runtime.active[i] == 1);
    }
    assert(runtime.rollbacks[7] == 1U);
    assert(runtime.active[7] == 1);
    for (size_t i = 8U; i <= 10U; ++i) {
        assert(runtime.rollbacks[i] == 1U);
        assert(runtime.active[i] == 0);
    }
    assert(runtime.rollbacks[11] == 0U);
    assert(runtime.active[11] == 0);
    assert(voice_assistant_bootstrap_lifecycle_start(&lifecycle, &runtime,
                                                      &rollback_error) == ESP_ERR_INVALID_STATE);
}

int main(void)
{
    const size_t representative_failures[] = { 0U, 1U, 3U, 7U, 8U, 9U, 10U, 11U };
    for (size_t i = 0; i < sizeof(representative_failures) / sizeof(representative_failures[0]); ++i) {
        test_failure_rolls_back_and_retries(representative_failures[i]);
    }
    test_cleanup_failure_blocks_retry();
    puts("voice bootstrap lifecycle transactional rollback: PASS");
    return 0;
}
