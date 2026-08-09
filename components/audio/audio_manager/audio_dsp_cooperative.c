/**
 * @file audio_dsp_cooperative.c
 * @brief Cooperative long-buffer variants of simple PCM24 DSP passes.
 */

#include "audio_dsp.h"

#include <stdint.h>

_Static_assert(AUDIO_DSP_FILTER_STAGE_COUNT == 2U,
               "This implementation expects two filter stages");
_Static_assert(AUDIO_DSP_COOPERATIVE_YIELD_INTERVAL_SAMPLES > 0U,
               "Cooperative sample-yield interval must be non-zero");

static uint32_t absolute_pcm24_local(int32_t sample)
{
    return (sample < 0) ? (uint32_t)(-(int64_t)sample) : (uint32_t)sample;
}

static int32_t saturate_pcm24_local(int64_t value)
{
    if (value > AUDIO_DSP_PCM24_POSITIVE_FULL_SCALE)
    {
        return AUDIO_DSP_PCM24_POSITIVE_FULL_SCALE;
    }
    if (value < AUDIO_DSP_PCM24_NEGATIVE_FULL_SCALE)
    {
        return AUDIO_DSP_PCM24_NEGATIVE_FULL_SCALE;
    }
    return (int32_t)value;
}

static void cooperative_yield_if_due(
    size_t completed_samples,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context)
{
    if ((yield_cb != NULL) &&
        ((completed_samples % AUDIO_DSP_COOPERATIVE_YIELD_INTERVAL_SAMPLES) == 0U))
    {
        yield_cb(yield_context);
    }
}

void audio_dsp_calculate_level_cooperative(
    const int32_t *samples,
    size_t sample_count,
    uint32_t *average_absolute,
    uint32_t *peak,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context)
{
    if ((average_absolute == NULL) || (peak == NULL))
    {
        return;
    }

    *average_absolute = 0U;
    *peak = 0U;
    if ((samples == NULL) || (sample_count == 0U))
    {
        return;
    }

    uint64_t absolute_sum = 0U;
    uint32_t local_peak = 0U;

    for (size_t index = 0U; index < sample_count; ++index)
    {
        const uint32_t magnitude = absolute_pcm24_local(samples[index]);
        absolute_sum += magnitude;
        if (magnitude > local_peak)
        {
            local_peak = magnitude;
        }

        cooperative_yield_if_due(index + 1U, yield_cb, yield_context);
    }

    *average_absolute = (uint32_t)(absolute_sum / sample_count);
    *peak = local_peak;
}

int32_t audio_dsp_calculate_dc_offset_cooperative(
    const int32_t *samples,
    size_t sample_count,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context)
{
    if ((samples == NULL) || (sample_count == 0U))
    {
        return 0;
    }

    int64_t sum = 0;
    for (size_t index = 0U; index < sample_count; ++index)
    {
        sum += samples[index];
        cooperative_yield_if_due(index + 1U, yield_cb, yield_context);
    }

    return (int32_t)(sum / (int64_t)sample_count);
}

static int32_t highpass_step_q15_local(
    int32_t input,
    int32_t *previous_input,
    int32_t *previous_output)
{
    const int64_t feedback =
        ((int64_t)AUDIO_DSP_HPF_ALPHA_Q15 * (int64_t)(*previous_output)) /
        32768;
    const int32_t output = saturate_pcm24_local(
        (int64_t)input - *previous_input + feedback);

    *previous_input = input;
    *previous_output = output;
    return output;
}

static int32_t lowpass_step_q15_local(
    int32_t input,
    int32_t *previous_output)
{
    const int64_t delta = (int64_t)input - (int64_t)(*previous_output);
    const int32_t output = saturate_pcm24_local(
        (int64_t)(*previous_output) +
        ((delta * AUDIO_DSP_LPF_BETA_Q15) / 32768));

    *previous_output = output;
    return output;
}

void audio_dsp_apply_speech_band_filter_in_place_cooperative(
    int32_t *samples,
    size_t sample_count,
    int32_t dc_offset,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context)
{
    if ((samples == NULL) || (sample_count == 0U))
    {
        return;
    }

    int32_t hp_previous_input[AUDIO_DSP_FILTER_STAGE_COUNT] = {0};
    int32_t hp_previous_output[AUDIO_DSP_FILTER_STAGE_COUNT] = {0};
    int32_t lp_previous_output[AUDIO_DSP_FILTER_STAGE_COUNT] = {0};

    for (size_t index = 0U; index < sample_count; ++index)
    {
        int32_t filtered = saturate_pcm24_local(
            (int64_t)samples[index] - dc_offset);

        for (size_t stage = 0U; stage < AUDIO_DSP_FILTER_STAGE_COUNT; ++stage)
        {
            filtered = highpass_step_q15_local(
                filtered,
                &hp_previous_input[stage],
                &hp_previous_output[stage]);
        }

        for (size_t stage = 0U; stage < AUDIO_DSP_FILTER_STAGE_COUNT; ++stage)
        {
            filtered = lowpass_step_q15_local(
                filtered,
                &lp_previous_output[stage]);
        }

        samples[index] = filtered;
        cooperative_yield_if_due(index + 1U, yield_cb, yield_context);
    }
}
