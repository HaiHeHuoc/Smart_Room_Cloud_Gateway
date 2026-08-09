#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MANAGER_DEFAULT_RECORD_SECONDS        5U
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
    UBaseType_t test_task_priority;
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

/** Return the fixed NewSolution defaults used by the stability harness. */
audio_manager_config_t audio_manager_default_config(void);

/**
 * Initialize the NewSolution audio pipeline and its PSRAM-owned recording/DSP
 * buffers. This does not start the infinite stability task.
 */
esp_err_t audio_manager_init(const audio_manager_config_t *config);

/**
 * Start one manager-owned task that repeatedly executes:
 * record -> DSP -> playback -> cleanup -> repeat.
 * Calling this again while already running is idempotent.
 */
esp_err_t audio_manager_test_start(void);

/** Copy a diagnostic snapshot. */
esp_err_t audio_manager_get_status(audio_manager_status_t *status);

/**
 * Deinitialize the manager only when the stability task is not running.
 * The current NewSolution intentionally runs the soak task for firmware life.
 */
esp_err_t audio_manager_deinit(void);

const char *audio_manager_state_to_string(audio_manager_state_t state);

#ifdef __cplusplus
}
#endif
