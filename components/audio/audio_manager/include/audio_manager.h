#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MANAGER_DEFAULT_RECORD_SECONDS        20U
#define AUDIO_MANAGER_DEFAULT_VOLUME_PERCENT        100U
#define AUDIO_MANAGER_DEFAULT_TEST_TASK_STACK_SIZE  8192U
#define AUDIO_MANAGER_DEFAULT_TEST_TASK_PRIORITY    5U
#define AUDIO_MANAGER_DEFAULT_INTER_CYCLE_DELAY_MS  250U

typedef enum
{
    AUDIO_MANAGER_STATE_UNINITIALIZED = 0,
    AUDIO_MANAGER_STATE_IDLE,
    AUDIO_MANAGER_STATE_RECORDING,
    AUDIO_MANAGER_STATE_PROCESSING,
    AUDIO_MANAGER_STATE_PLAYBACK,
    AUDIO_MANAGER_STATE_ERROR,
} audio_manager_state_t;

typedef struct
{
    uint32_t record_duration_seconds;
    uint32_t playback_volume_percent;
    uint32_t test_task_stack_size;
    uint32_t test_task_priority;
    uint32_t inter_cycle_delay_ms;
} audio_manager_config_t;

typedef struct
{
    audio_manager_state_t state;
    bool initialized;
    bool test_running;

    uint32_t cycles_started;
    uint32_t cycles_completed;
    uint32_t cycles_failed;
    esp_err_t last_error;

    size_t last_samples_recorded;
    uint32_t rx_overflow_count;
    uint32_t rx_timeout_count;
    uint32_t tx_queue_overflow_count;
    uint32_t tx_timeout_count;
    uint32_t tx_partial_write_count;

    uint32_t max_rx_read_duration_us;
    uint32_t max_tx_write_duration_us;
    uint32_t test_task_stack_high_water_bytes;
} audio_manager_status_t;

audio_manager_config_t audio_manager_default_config(void);
esp_err_t audio_manager_init(const audio_manager_config_t *config);
esp_err_t audio_manager_test_start(void);
esp_err_t audio_manager_get_status(audio_manager_status_t *status);
esp_err_t audio_manager_deinit(void);
const char *audio_manager_state_to_string(audio_manager_state_t state);

#ifdef __cplusplus
}
#endif
