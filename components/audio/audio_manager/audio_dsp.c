#include "audio_dsp.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

_Static_assert((AUDIO_DSP_NS_FFT_SIZE & (AUDIO_DSP_NS_FFT_SIZE - 1U)) == 0U,
               "FFT size must be power of two");
_Static_assert(AUDIO_DSP_NS_HOP_SIZE * 2U == AUDIO_DSP_NS_FFT_SIZE,
               "Spectral NS expects 50 percent overlap");
_Static_assert((1U << AUDIO_DSP_NS_FFT_STAGE_COUNT) == AUDIO_DSP_NS_FFT_SIZE,
               "FFT stage count must match FFT size");
_Static_assert(AUDIO_DSP_NS_MIN_WINDOW_FRAMES > 0U,
               "Minimum-statistics window must contain frames");
_Static_assert(AUDIO_DSP_PLAYBACK_GAIN_Q8 > 0,
               "Playback gain must be positive");
_Static_assert(AUDIO_DSP_PEAK_SOFT_THRESHOLD_PCM16 <
                   AUDIO_DSP_PLAYBACK_VALIDATED_PEAK_PCM16,
               "Peak soft threshold must be below output ceiling");
_Static_assert(AUDIO_DSP_PLAYBACK_VALIDATED_PEAK_PCM16 < INT16_MAX,
               "Speaker preview ceiling must fit PCM16");
_Static_assert(AUDIO_DSP_PLAYBACK_FADE_SAMPLES > 0U,
               "Playback fade must contain samples");

static int32_t saturate_pcm24(int64_t value)
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

int32_t audio_dsp_convert_raw_slot_to_pcm24(int32_t raw_slot)
{
    uint32_t sample = ((uint32_t)raw_slot >> 8U) & 0x00FFFFFFU;
    if ((sample & 0x00800000U) != 0U)
    {
        sample |= 0xFF000000U;
    }
    return (int32_t)sample;
}

void audio_dsp_workspace_init(audio_dsp_workspace_t *workspace)
{
    if (workspace == NULL)
    {
        return;
    }

    memset(workspace, 0, sizeof(*workspace));

    for (size_t index = 0U; index < AUDIO_DSP_NS_FFT_SIZE; ++index)
    {
        const float hann =
            0.5f -
            0.5f * cosf((2.0f * PI_F * (float)index) /
                        (float)AUDIO_DSP_NS_FFT_SIZE);
        workspace->window[index] = sqrtf((hann > 0.0f) ? hann : 0.0f);
    }

    size_t stage = 0U;
    for (size_t length = 2U;
         length <= AUDIO_DSP_NS_FFT_SIZE;
         length <<= 1U, ++stage)
    {
        const float angle = -2.0f * PI_F / (float)length;
        workspace->fft_step_real[stage] = cosf(angle);
        workspace->fft_step_imag_forward[stage] = sinf(angle);
    }

    workspace->tables_initialized = true;
    audio_dsp_workspace_reset_recording(workspace);
}

void audio_dsp_workspace_reset_recording(audio_dsp_workspace_t *workspace)
{
    if (workspace == NULL)
    {
        return;
    }

    memset(workspace->real, 0, sizeof(workspace->real));
    memset(workspace->imag, 0, sizeof(workspace->imag));
    memset(workspace->overlap, 0, sizeof(workspace->overlap));
    memset(workspace->smoothed_power, 0, sizeof(workspace->smoothed_power));
    memset(workspace->minimum_power, 0, sizeof(workspace->minimum_power));
    memset(workspace->noise_power, 0, sizeof(workspace->noise_power));
    memset(workspace->eligible_noise_frames, 0, sizeof(workspace->eligible_noise_frames));

    for (size_t bin = 0U; bin < AUDIO_DSP_NS_BIN_COUNT; ++bin)
    {
        workspace->previous_gain[bin] = 1.0f;
    }

    workspace->frames_in_minimum_window = 0U;
    workspace->noise_updates = 0U;
    workspace->power_initialized = false;
    workspace->noise_initialized = false;
}

static void fft_in_place(
    float *real,
    float *imag,
    bool inverse,
    const audio_dsp_workspace_t *workspace)
{
    for (size_t i = 1U, j = 0U; i < AUDIO_DSP_NS_FFT_SIZE; ++i)
    {
        size_t bit = AUDIO_DSP_NS_FFT_SIZE >> 1U;
        for (; (j & bit) != 0U; bit >>= 1U)
        {
            j ^= bit;
        }
        j ^= bit;

        if (i < j)
        {
            const float real_temp = real[i];
            real[i] = real[j];
            real[j] = real_temp;

            const float imag_temp = imag[i];
            imag[i] = imag[j];
            imag[j] = imag_temp;
        }
    }

    size_t stage = 0U;
    for (size_t length = 2U;
         length <= AUDIO_DSP_NS_FFT_SIZE;
         length <<= 1U, ++stage)
    {
        const float step_real = workspace->fft_step_real[stage];
        const float step_imag = inverse
            ? -workspace->fft_step_imag_forward[stage]
            : workspace->fft_step_imag_forward[stage];

        for (size_t base = 0U; base < AUDIO_DSP_NS_FFT_SIZE; base += length)
        {
            float twiddle_real = 1.0f;
            float twiddle_imag = 0.0f;
            const size_t half = length >> 1U;

            for (size_t offset = 0U; offset < half; ++offset)
            {
                const size_t even_index = base + offset;
                const size_t odd_index = even_index + half;

                const float odd_real =
                    (real[odd_index] * twiddle_real) -
                    (imag[odd_index] * twiddle_imag);
                const float odd_imag =
                    (real[odd_index] * twiddle_imag) +
                    (imag[odd_index] * twiddle_real);

                const float even_real = real[even_index];
                const float even_imag = imag[even_index];

                real[even_index] = even_real + odd_real;
                imag[even_index] = even_imag + odd_imag;
                real[odd_index] = even_real - odd_real;
                imag[odd_index] = even_imag - odd_imag;

                const float next_twiddle_real =
                    (twiddle_real * step_real) -
                    (twiddle_imag * step_imag);
                twiddle_imag =
                    (twiddle_real * step_imag) +
                    (twiddle_imag * step_real);
                twiddle_real = next_twiddle_real;
            }
        }
    }

    if (inverse)
    {
        const float inverse_size = 1.0f / (float)AUDIO_DSP_NS_FFT_SIZE;
        for (size_t index = 0U; index < AUDIO_DSP_NS_FFT_SIZE; ++index)
        {
            real[index] *= inverse_size;
            imag[index] *= inverse_size;
        }
    }
}

static void load_fft_frame(
    const int32_t *samples,
    size_t sample_count,
    size_t frame_start,
    audio_dsp_workspace_t *workspace)
{
    for (size_t index = 0U; index < AUDIO_DSP_NS_FFT_SIZE; ++index)
    {
        const size_t sample_index = frame_start + index;
        const float pcm16 =
            (sample_index < sample_count)
                ? ((float)samples[sample_index] /
                   (float)AUDIO_DSP_PCM24_SCALE_FACTOR)
                : 0.0f;

        workspace->real[index] = pcm16 * workspace->window[index];
        workspace->imag[index] = 0.0f;
    }
}

static bool bin_has_speech_presence(
    const audio_dsp_workspace_t *workspace,
    size_t bin,
    float signal_power)
{
    if (!workspace->noise_initialized)
    {
        return false;
    }

    const float noise_power = workspace->noise_power[bin];
    if (noise_power <= AUDIO_DSP_NS_EPSILON)
    {
        return false;
    }

    return signal_power >
        (noise_power * AUDIO_DSP_NS_SPEECH_PROTECT_RATIO);
}

static void observe_power_for_noise_tracker(
    audio_dsp_workspace_t *workspace,
    size_t bin,
    float signal_power,
    bool speech_present)
{
    if (!workspace->power_initialized)
    {
        workspace->smoothed_power[bin] = signal_power;
        workspace->minimum_power[bin] = signal_power;
        workspace->eligible_noise_frames[bin] = 1U;
        return;
    }

    const float smoothed =
        (AUDIO_DSP_NS_POWER_SMOOTHING * workspace->smoothed_power[bin]) +
        ((1.0f - AUDIO_DSP_NS_POWER_SMOOTHING) * signal_power);

    workspace->smoothed_power[bin] = smoothed;

    if (speech_present)
    {
        return;
    }

    if (workspace->eligible_noise_frames[bin] < UINT16_MAX)
    {
        ++workspace->eligible_noise_frames[bin];
    }

    if ((workspace->minimum_power[bin] <= AUDIO_DSP_NS_EPSILON) ||
        (smoothed < workspace->minimum_power[bin]))
    {
        workspace->minimum_power[bin] = smoothed;
    }
}

static bool finish_noise_tracker_frame(audio_dsp_workspace_t *workspace)
{
    workspace->power_initialized = true;
    ++workspace->frames_in_minimum_window;

    if (workspace->frames_in_minimum_window < AUDIO_DSP_NS_MIN_WINDOW_FRAMES)
    {
        return false;
    }

    bool any_bin_updated = false;

    for (size_t bin = 0U; bin < AUDIO_DSP_NS_BIN_COUNT; ++bin)
    {
        const bool has_noise_candidate =
            !workspace->noise_initialized ||
            (workspace->eligible_noise_frames[bin] > 0U);

        if (has_noise_candidate)
        {
            float candidate =
                workspace->minimum_power[bin] * AUDIO_DSP_NS_MINIMUM_BIAS;
            if (candidate < AUDIO_DSP_NS_EPSILON)
            {
                candidate = AUDIO_DSP_NS_EPSILON;
            }

            if (!workspace->noise_initialized)
            {
                workspace->noise_power[bin] = candidate;
            }
            else
            {
                const float previous = workspace->noise_power[bin];
                const float smoothing =
                    (candidate > previous)
                        ? AUDIO_DSP_NS_NOISE_RISE_SMOOTHING
                        : AUDIO_DSP_NS_NOISE_FALL_SMOOTHING;

                workspace->noise_power[bin] =
                    (smoothing * previous) +
                    ((1.0f - smoothing) * candidate);
            }

            any_bin_updated = true;
        }

        workspace->minimum_power[bin] = workspace->smoothed_power[bin];
        workspace->eligible_noise_frames[bin] = 0U;
    }

    workspace->noise_initialized = true;
    workspace->frames_in_minimum_window = 0U;
    if (any_bin_updated)
    {
        ++workspace->noise_updates;
    }
    return any_bin_updated;
}

static float spectral_gain_for_bin(
    float signal_power,
    float noise_power,
    float previous_gain)
{
    if (noise_power <= AUDIO_DSP_NS_EPSILON)
    {
        return 1.0f;
    }

    float gain =
        1.0f -
        ((AUDIO_DSP_NS_OVERSUBTRACTION * noise_power) /
         (signal_power + AUDIO_DSP_NS_EPSILON));

    if (gain < AUDIO_DSP_NS_GAIN_FLOOR)
    {
        gain = AUDIO_DSP_NS_GAIN_FLOOR;
    }
    else if (gain > 1.0f)
    {
        gain = 1.0f;
    }

    gain =
        (AUDIO_DSP_NS_GAIN_SMOOTHING * previous_gain) +
        ((1.0f - AUDIO_DSP_NS_GAIN_SMOOTHING) * gain);

    if (gain < AUDIO_DSP_NS_GAIN_FLOOR)
    {
        gain = AUDIO_DSP_NS_GAIN_FLOOR;
    }
    if (gain > 1.0f)
    {
        gain = 1.0f;
    }
    return gain;
}

static int32_t float_pcm16_to_pcm24(float pcm16)
{
    if (pcm16 > 32767.0f)
    {
        pcm16 = 32767.0f;
    }
    else if (pcm16 < -32768.0f)
    {
        pcm16 = -32768.0f;
    }

    return saturate_pcm24(
        (int64_t)(pcm16 * (float)AUDIO_DSP_PCM24_SCALE_FACTOR));
}

bool audio_dsp_apply_adaptive_ns_in_place_cooperative(
    int32_t *samples,
    size_t sample_count,
    audio_dsp_workspace_t *workspace,
    audio_dsp_ns_metrics_t *metrics,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context)
{
    if ((samples == NULL) ||
        (sample_count < AUDIO_DSP_NS_FFT_SIZE) ||
        (workspace == NULL))
    {
        return false;
    }

    if (!workspace->tables_initialized)
    {
        audio_dsp_workspace_init(workspace);
    }
    else
    {
        audio_dsp_workspace_reset_recording(workspace);
    }

    audio_dsp_ns_metrics_t local_metrics = {0};
    local_metrics.minimum_gain = 1.0f;
    local_metrics.maximum_gain = 0.0f;
    float gain_sum = 0.0f;

    for (size_t frame_start = 0U;
         frame_start < sample_count;
         frame_start += AUDIO_DSP_NS_HOP_SIZE)
    {
        load_fft_frame(samples, sample_count, frame_start, workspace);
        fft_in_place(workspace->real, workspace->imag, false, workspace);

        for (size_t bin = 0U; bin < AUDIO_DSP_NS_BIN_COUNT; ++bin)
        {
            const float real = workspace->real[bin];
            const float imag = workspace->imag[bin];
            const float signal_power = (real * real) + (imag * imag);
            const bool speech_present =
                bin_has_speech_presence(workspace, bin, signal_power);

            observe_power_for_noise_tracker(
                workspace,
                bin,
                signal_power,
                speech_present);

            if (speech_present)
            {
                ++local_metrics.speech_protected_bin_frames;
            }
        }

        (void)finish_noise_tracker_frame(workspace);

        for (size_t bin = 0U; bin < AUDIO_DSP_NS_BIN_COUNT; ++bin)
        {
            const float real = workspace->real[bin];
            const float imag = workspace->imag[bin];
            const float signal_power = (real * real) + (imag * imag);

            const float gain =
                workspace->noise_initialized
                    ? spectral_gain_for_bin(
                          signal_power,
                          workspace->noise_power[bin],
                          workspace->previous_gain[bin])
                    : 1.0f;

            workspace->previous_gain[bin] = gain;
            workspace->real[bin] *= gain;
            workspace->imag[bin] *= gain;

            if ((bin > 0U) && (bin < (AUDIO_DSP_NS_FFT_SIZE / 2U)))
            {
                const size_t mirror = AUDIO_DSP_NS_FFT_SIZE - bin;
                workspace->real[mirror] *= gain;
                workspace->imag[mirror] *= gain;
            }

            gain_sum += gain;
            ++local_metrics.processed_bin_frames;
            if (gain <= (AUDIO_DSP_NS_GAIN_FLOOR + 1.0e-4f))
            {
                ++local_metrics.floor_bin_frames;
            }
            if (gain < local_metrics.minimum_gain)
            {
                local_metrics.minimum_gain = gain;
            }
            if (gain > local_metrics.maximum_gain)
            {
                local_metrics.maximum_gain = gain;
            }
        }

        fft_in_place(workspace->real, workspace->imag, true, workspace);

        for (size_t index = 0U; index < AUDIO_DSP_NS_HOP_SIZE; ++index)
        {
            const size_t sample_index = frame_start + index;
            const float current_first =
                workspace->real[index] * workspace->window[index];
            const float finalized = workspace->overlap[index] + current_first;

            if (sample_index < sample_count)
            {
                samples[sample_index] = float_pcm16_to_pcm24(finalized);
            }

            workspace->overlap[index] =
                workspace->real[index + AUDIO_DSP_NS_HOP_SIZE] *
                workspace->window[index + AUDIO_DSP_NS_HOP_SIZE];
        }

        ++local_metrics.processed_frames;

        if ((yield_cb != NULL) &&
            ((local_metrics.processed_frames %
              AUDIO_DSP_COOPERATIVE_YIELD_INTERVAL_FRAMES) == 0U))
        {
            yield_cb(yield_context);
            ++local_metrics.cooperative_yields;
        }
    }

    local_metrics.noise_updates = workspace->noise_updates;
    if (local_metrics.processed_bin_frames > 0U)
    {
        local_metrics.average_gain =
            gain_sum / (float)local_metrics.processed_bin_frames;
    }
    else
    {
        local_metrics.minimum_gain = 0.0f;
    }

    if (metrics != NULL)
    {
        *metrics = local_metrics;
    }
    return true;
}

bool audio_dsp_apply_adaptive_ns_in_place(
    int32_t *samples,
    size_t sample_count,
    audio_dsp_workspace_t *workspace,
    audio_dsp_ns_metrics_t *metrics)
{
    return audio_dsp_apply_adaptive_ns_in_place_cooperative(
        samples,
        sample_count,
        workspace,
        metrics,
        NULL,
        NULL);
}

static int32_t apply_memoryless_peak_protection(
    int32_t sample,
    bool *soft_compressed,
    bool *limited)
{
    const bool negative = (sample < 0);
    int64_t magnitude = negative ? -(int64_t)sample : (int64_t)sample;
    bool compressed = false;
    bool clipped = false;

    if (magnitude > AUDIO_DSP_PEAK_SOFT_THRESHOLD_PCM16)
    {
        magnitude = AUDIO_DSP_PEAK_SOFT_THRESHOLD_PCM16 +
                    ((magnitude - AUDIO_DSP_PEAK_SOFT_THRESHOLD_PCM16) /
                     AUDIO_DSP_PEAK_SOFT_RATIO);
        compressed = true;
    }

    if (magnitude > AUDIO_DSP_PLAYBACK_VALIDATED_PEAK_PCM16)
    {
        magnitude = AUDIO_DSP_PLAYBACK_VALIDATED_PEAK_PCM16;
        clipped = true;
    }

    if (soft_compressed != NULL)
    {
        *soft_compressed = compressed;
    }
    if (limited != NULL)
    {
        *limited = clipped;
    }

    return negative ? -(int32_t)magnitude : (int32_t)magnitude;
}

int16_t audio_dsp_prepare_output_sample(
    int32_t sample_pcm24,
    size_t sample_index,
    size_t sample_count,
    bool *soft_compressed,
    bool *limited)
{
    if ((sample_count == 0U) || (sample_index >= sample_count))
    {
        if (soft_compressed != NULL)
        {
            *soft_compressed = false;
        }
        if (limited != NULL)
        {
            *limited = false;
        }
        return 0;
    }

    int64_t scaled_pcm24 =
        ((int64_t)sample_pcm24 * AUDIO_DSP_PLAYBACK_GAIN_Q8) /
        AUDIO_DSP_Q8_ONE;

    uint32_t envelope_q15 = AUDIO_DSP_Q15_FULL_SCALE;
    if (sample_index < AUDIO_DSP_PLAYBACK_FADE_SAMPLES)
    {
        envelope_q15 =
            (uint32_t)(((sample_index + 1U) * AUDIO_DSP_Q15_FULL_SCALE) /
                       AUDIO_DSP_PLAYBACK_FADE_SAMPLES);
    }

    const size_t samples_remaining = sample_count - sample_index;
    if (samples_remaining <= AUDIO_DSP_PLAYBACK_FADE_SAMPLES)
    {
        const uint32_t fade_out_q15 =
            (uint32_t)(((samples_remaining - 1U) * AUDIO_DSP_Q15_FULL_SCALE) /
                       AUDIO_DSP_PLAYBACK_FADE_SAMPLES);
        if (fade_out_q15 < envelope_q15)
        {
            envelope_q15 = fade_out_q15;
        }
    }

    scaled_pcm24 =
        (scaled_pcm24 * envelope_q15) / AUDIO_DSP_Q15_FULL_SCALE;

    int32_t pcm16 =
        (int32_t)(scaled_pcm24 / AUDIO_DSP_PCM24_SCALE_FACTOR);
    pcm16 = apply_memoryless_peak_protection(
        pcm16,
        soft_compressed,
        limited);
    return (int16_t)pcm16;
}
