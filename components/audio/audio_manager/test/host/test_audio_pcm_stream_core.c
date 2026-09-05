#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_manager_pcm_stream_core.h"

static bool test_copy_ownership_and_wrap(void)
{
    int16_t storage[8] = {0};
    int16_t first[] = {1, 2, 3, 4};
    int16_t second[] = {5, 6, 7, 8, 9};
    int16_t output[8] = {0};
    audio_manager_pcm_stream_core_t core = {0};
    size_t read = 0U;

    if ((audio_manager_pcm_stream_core_init(&core, storage, 8U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_prepare(&core, 11U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_write(&core, 11U, first, 4U) != ESP_OK)) {
        return false;
    }
    first[0] = 99;
    if ((audio_manager_pcm_stream_core_read(&core, 11U, output, 2U, &read) != ESP_OK) ||
        (read != 2U) || (output[0] != 1) || (output[1] != 2)) {
        return false;
    }
    if (audio_manager_pcm_stream_core_write(&core, 11U, second, 5U) != ESP_OK) {
        return false;
    }
    if ((audio_manager_pcm_stream_core_read(&core, 11U, output, 8U, &read) != ESP_OK) ||
        (read != 7U)) {
        return false;
    }

    const int16_t expected[] = {3, 4, 5, 6, 7, 8, 9};
    for (size_t index = 0U; index < read; ++index) {
        if (output[index] != expected[index]) {
            return false;
        }
    }
    return (core.accepted_samples == 9U) && (core.consumed_samples == 9U) &&
           (core.high_water_samples == 7U);
}

static bool test_full_packet_backpressure_and_stale_generation(void)
{
    int16_t storage[4] = {0};
    const int16_t accepted[] = {10, 11, 12};
    const int16_t rejected[] = {13, 14};
    int16_t output[4] = {0};
    audio_manager_pcm_stream_core_t core = {0};
    size_t read = 0U;

    if ((audio_manager_pcm_stream_core_init(&core, storage, 4U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_prepare(&core, 21U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_write(&core, 21U, accepted, 3U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_write(&core, 21U, rejected, 2U) != ESP_ERR_TIMEOUT) ||
        (audio_manager_pcm_stream_core_write(&core, 22U, rejected, 1U) != ESP_ERR_INVALID_STATE) ||
        (core.full_count != 1U)) {
        return false;
    }
    if ((audio_manager_pcm_stream_core_read(&core, 21U, output, 4U, &read) != ESP_OK) ||
        (read != 3U)) {
        return false;
    }
    return (output[0] == 10) && (output[1] == 11) && (output[2] == 12) &&
           (core.accepted_samples == 3U) && (core.consumed_samples == 3U);
}

static bool test_eos_and_abort_do_not_leak_between_generations(void)
{
    int16_t storage[6] = {0};
    const int16_t first[] = {1, 2};
    const int16_t second[] = {7, 8, 9};
    int16_t output[6] = {0};
    audio_manager_pcm_stream_core_t core = {0};
    audio_manager_pcm_stream_core_t snapshot = {0};
    size_t read = 0U;

    if ((audio_manager_pcm_stream_core_init(&core, storage, 6U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_prepare(&core, 31U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_write(&core, 31U, first, 2U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_finish(&core, 31U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_write(&core, 31U, second, 1U) != ESP_ERR_INVALID_STATE) ||
        (audio_manager_pcm_stream_core_abort(&core, 31U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_close(&core, 31U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_get(&core, 31U, &snapshot) != ESP_OK) ||
        snapshot.active || !snapshot.aborted || (snapshot.queued_samples != 0U)) {
        return false;
    }
    if ((audio_manager_pcm_stream_core_prepare(&core, 32U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_write(&core, 32U, second, 3U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_finish(&core, 32U) != ESP_OK) ||
        (audio_manager_pcm_stream_core_read(&core, 32U, output, 6U, &read) != ESP_OK) ||
        (read != 3U)) {
        return false;
    }
    return (output[0] == 7) && (output[1] == 8) && (output[2] == 9) &&
           (core.accepted_samples == 3U) && (core.consumed_samples == 3U);
}

int main(void)
{
    const bool copy_wrap_ok = test_copy_ownership_and_wrap();
    const bool backpressure_ok = test_full_packet_backpressure_and_stale_generation();
    const bool eos_abort_ok = test_eos_and_abort_do_not_leak_between_generations();

    printf("[%s] PCM stream copy ownership and wraparound\n",
           copy_wrap_ok ? "PASS" : "FAIL");
    printf("[%s] PCM stream atomic backpressure and stale generation\n",
           backpressure_ok ? "PASS" : "FAIL");
    printf("[%s] PCM stream EOS and abort generation isolation\n",
           eos_abort_ok ? "PASS" : "FAIL");
    return (copy_wrap_ok && backpressure_ok && eos_abort_ok) ? 0 : 1;
}
