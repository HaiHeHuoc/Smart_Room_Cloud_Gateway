/**
 * @file audio_wav_prefetch.h
 * @brief Private bounded SD-to-PSRAM WAV prefetcher for audio_manager.
 *
 * The prefetcher owns its WAV stream, FILE, and SD VFS lease.  It has no I2S
 * or LVGL dependency: the audio-manager task consumes only READY PCM bytes
 * from its PSRAM slots and remains the sole I2S owner.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_manager.h"
#include "audio_wav.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/** Two slots provide one logical block of playback margin while the other fills. */
#define AUDIO_WAV_PREFETCH_SLOT_COUNT  2U

/** Slot value used by a terminal-error queue item that has no PCM data. */
#define AUDIO_WAV_PREFETCH_INVALID_SLOT  UINT8_MAX

/** @brief One READY prefetch block or a terminal reader error. */
typedef struct
{
    uint8_t slot_index;
    size_t valid_bytes;
    bool final_block;
    esp_err_t result;
} audio_wav_prefetch_item_t;

/** @brief Private reader metrics copied after its task has stopped. */
typedef struct
{
    uint64_t data_bytes_read;
    uint32_t io_read_count;
    uint32_t io_read_failure_count;
    uint32_t max_io_read_duration_us;
    uint32_t blocks_filled;
    uint32_t fill_failure_count;
    uint32_t max_fill_duration_us;
    uint64_t last_recovery_data_offset;
    uint32_t recovery_attempt_count;
    uint32_t recovery_success_count;
    uint32_t recovery_wait_ms;
    uint32_t task_stack_high_water;
} audio_wav_prefetch_metrics_t;

typedef struct
{
    uint8_t *data;
} audio_wav_prefetch_slot_t;

/**
 * @brief Private per-playback prefetch context.
 *
 * It is zero-initialized by the audio-manager source slot.  The producer owns
 * stream and may access it until it signals stopped.  The manager must join
 * the producer before destroying this context or beginning another WAV.
 */
typedef struct
{
    audio_wav_prefetch_slot_t slots[AUDIO_WAV_PREFETCH_SLOT_COUNT];
    size_t slot_bytes;
    QueueHandle_t free_slots;
    QueueHandle_t ready_items;
    EventGroupHandle_t events;
    TaskHandle_t task_handle;
    TaskHandle_t owner_task;
    portMUX_TYPE lock;

    char path[AUDIO_MANAGER_WAV_PATH_MAX_BYTES];
    audio_wav_stream_t stream;
    audio_wav_info_t info;
    audio_wav_prefetch_metrics_t metrics;
    esp_err_t worker_result;
    bool info_valid;
    bool started;
    bool stop_requested;
    bool worker_stopped;
    bool destroy_ack_sent;
} audio_wav_prefetch_t;

/** Start one private reader with two PSRAM logical PCM slots. */
esp_err_t audio_wav_prefetch_start(
    audio_wav_prefetch_t *prefetch,
    const char *path,
    size_t slot_bytes,
    UBaseType_t task_priority);

/** Poll or wait for a producer-ready PCM block or terminal reader error. */
BaseType_t audio_wav_prefetch_take_ready(
    audio_wav_prefetch_t *prefetch,
    audio_wav_prefetch_item_t *item,
    TickType_t timeout);

/** Return a consumed READY slot to the producer. */
esp_err_t audio_wav_prefetch_release_slot(
    audio_wav_prefetch_t *prefetch,
    uint8_t slot_index);

/** Return immutable PCM data for a READY/consuming slot. */
const uint8_t *audio_wav_prefetch_slot_data(
    const audio_wav_prefetch_t *prefetch,
    uint8_t slot_index);

/** Return parsed WAV metadata after a successful READY item is received. */
esp_err_t audio_wav_prefetch_get_info(
    const audio_wav_prefetch_t *prefetch,
    audio_wav_info_t *info);

/** Request that the reader stop after its current bounded raw read. */
void audio_wav_prefetch_request_stop(audio_wav_prefetch_t *prefetch);

/** Wait for the worker to release its WAV stream and SD VFS lease. */
esp_err_t audio_wav_prefetch_wait_stopped(
    audio_wav_prefetch_t *prefetch,
    TickType_t timeout);

/** Read the terminal worker result after audio_wav_prefetch_wait_stopped(). */
esp_err_t audio_wav_prefetch_get_worker_result(
    const audio_wav_prefetch_t *prefetch);

/** Copy worker metrics after audio_wav_prefetch_wait_stopped() succeeds. */
esp_err_t audio_wav_prefetch_get_metrics(
    const audio_wav_prefetch_t *prefetch,
    audio_wav_prefetch_metrics_t *metrics);

/**
 * Join, acknowledge the worker's event-group exit, then free private
 * queues/events/PSRAM. On timeout the context is intentionally retained so a
 * caller can keep draining safely and retry. This private API must run in the
 * task that started the prefetcher, which is the audio-manager task. Once all
 * resources are freed it returns ESP_OK regardless of the reader's operation
 * result; retrieve that separately with audio_wav_prefetch_get_worker_result().
 */
esp_err_t audio_wav_prefetch_stop_and_destroy(
    audio_wav_prefetch_t *prefetch,
    TickType_t timeout);

/** True while a prefetch context owns resources or a worker task. */
bool audio_wav_prefetch_is_active(const audio_wav_prefetch_t *prefetch);
