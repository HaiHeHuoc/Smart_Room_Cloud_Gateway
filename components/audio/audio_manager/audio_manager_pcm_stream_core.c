#include "audio_manager_pcm_stream_core.h"

#include <string.h>

static bool core_matches(const audio_manager_pcm_stream_core_t *core,
                         uint32_t generation)
{
    return (core != NULL) && (generation != 0U) && core->active &&
           (core->generation == generation);
}

static void core_reset_indices(audio_manager_pcm_stream_core_t *core)
{
    core->read_index = 0U;
    core->write_index = 0U;
    core->queued_samples = 0U;
}

esp_err_t audio_manager_pcm_stream_core_init(
    audio_manager_pcm_stream_core_t *core,
    int16_t *storage,
    size_t capacity_samples)
{
    if ((core == NULL) || (storage == NULL) || (capacity_samples == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    *core = (audio_manager_pcm_stream_core_t) {
        .storage = storage,
        .capacity_samples = capacity_samples,
    };
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_core_prepare(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation)
{
    if ((core == NULL) || (core->storage == NULL) ||
        (core->capacity_samples == 0U) || (generation == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    core_reset_indices(core);
    core->generation = generation;
    core->active = true;
    core->ended = false;
    core->aborted = false;
    core->accepted_samples = 0U;
    core->consumed_samples = 0U;
    core->high_water_samples = 0U;
    core->full_count = 0U;
    core->starvation_count = 0U;
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_core_write(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation,
    const int16_t *samples,
    size_t sample_count)
{
    if ((samples == NULL) || (sample_count == 0U) ||
        (sample_count > AUDIO_MANAGER_PCM_STREAM_MAX_WRITE_SAMPLES)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!core_matches(core, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->ended || core->aborted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count > (core->capacity_samples - core->queued_samples)) {
        ++core->full_count;
        return ESP_ERR_TIMEOUT;
    }

    const size_t first =
        ((core->capacity_samples - core->write_index) < sample_count)
            ? (core->capacity_samples - core->write_index)
            : sample_count;
    memcpy(&core->storage[core->write_index], samples, first * sizeof(*samples));
    if (first < sample_count) {
        memcpy(core->storage,
               &samples[first],
               (sample_count - first) * sizeof(*samples));
    }

    core->write_index = (core->write_index + sample_count) %
                        core->capacity_samples;
    core->queued_samples += sample_count;
    core->accepted_samples += sample_count;
    if (core->queued_samples > core->high_water_samples) {
        core->high_water_samples = (uint32_t)core->queued_samples;
    }
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_core_read(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation,
    int16_t *samples,
    size_t capacity_samples,
    size_t *samples_read)
{
    if ((samples == NULL) || (capacity_samples == 0U) ||
        (samples_read == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *samples_read = 0U;
    if (!core_matches(core, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->aborted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core->queued_samples == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    const size_t count = (core->queued_samples < capacity_samples)
        ? core->queued_samples
        : capacity_samples;
    const size_t first = ((core->capacity_samples - core->read_index) < count)
        ? (core->capacity_samples - core->read_index)
        : count;
    memcpy(samples, &core->storage[core->read_index], first * sizeof(*samples));
    if (first < count) {
        memcpy(&samples[first], core->storage, (count - first) * sizeof(*samples));
    }

    core->read_index = (core->read_index + count) % core->capacity_samples;
    core->queued_samples -= count;
    core->consumed_samples += count;
    *samples_read = count;
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_core_finish(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation)
{
    if (!core_matches(core, generation) || core->aborted) {
        return ESP_ERR_INVALID_STATE;
    }
    core->ended = true;
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_core_abort(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation)
{
    if (!core_matches(core, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    core_reset_indices(core);
    core->aborted = true;
    core->ended = true;
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_core_close(
    audio_manager_pcm_stream_core_t *core,
    uint32_t generation)
{
    if (!core_matches(core, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    core_reset_indices(core);
    core->active = false;
    return ESP_OK;
}

esp_err_t audio_manager_pcm_stream_core_get(
    const audio_manager_pcm_stream_core_t *core,
    uint32_t generation,
    audio_manager_pcm_stream_core_t *snapshot)
{
    if ((core == NULL) || (snapshot == NULL) || (generation == 0U) ||
        (core->generation != generation)) {
        return ESP_ERR_NOT_FOUND;
    }
    *snapshot = *core;
    snapshot->storage = NULL;
    return ESP_OK;
}
