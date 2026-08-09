#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_DSP_SAMPLE_RATE_HZ               16000U
#define AUDIO_DSP_PCM24_SCALE_FACTOR            256U
#define AUDIO_DSP_PCM24_POSITIVE_FULL_SCALE     8388607
#define AUDIO_DSP_PCM24_NEGATIVE_FULL_SCALE     (-8388608)

#define AUDIO_DSP_PLAYBACK_GAIN_Q8              4096
#define AUDIO_DSP_Q8_ONE                        256

#define AUDIO_DSP_HPF_ALPHA_Q15                 31755
#define AUDIO_DSP_LPF_BETA_Q15                  29662
#define AUDIO_DSP_FILTER_STAGE_COUNT             2U

#define AUDIO_DSP_NS_FFT_SIZE                   256U
#define AUDIO_DSP_NS_HOP_SIZE                   (AUDIO_DSP_NS_FFT_SIZE / 2U)
#define AUDIO_DSP_NS_BIN_COUNT                  ((AUDIO_DSP_NS_FFT_SIZE / 2U) + 1U)
#define AUDIO_DSP_NS_FFT_STAGE_COUNT            8U
#define AUDIO_DSP_NS_MIN_WINDOW_FRAMES          64U
#define AUDIO_DSP_NS_POWER_SMOOTHING            0.80f
#define AUDIO_DSP_NS_MINIMUM_BIAS               1.25f
#define AUDIO_DSP_NS_NOISE_RISE_SMOOTHING       0.90f
#define AUDIO_DSP_NS_NOISE_FALL_SMOOTHING       0.50f
#define AUDIO_DSP_NS_OVERSUBTRACTION            1.50f
#define AUDIO_DSP_NS_GAIN_FLOOR                 0.20f
#define AUDIO_DSP_NS_GAIN_SMOOTHING             0.80f
#define AUDIO_DSP_NS_SPEECH_PROTECT_RATIO       4.00f
#define AUDIO_DSP_NS_EPSILON                    1.0e-9f

#define AUDIO_DSP_COOPERATIVE_YIELD_INTERVAL_SAMPLES 65536U
#define AUDIO_DSP_COOPERATIVE_YIELD_INTERVAL_FRAMES  256U

#define AUDIO_DSP_PEAK_SOFT_THRESHOLD_PCM16     6000
#define AUDIO_DSP_PEAK_SOFT_RATIO               4
#define AUDIO_DSP_PLAYBACK_PEAK_LIMIT_PCM16     8500
#define AUDIO_DSP_PLAYBACK_VALIDATED_PEAK_PCM16 AUDIO_DSP_PLAYBACK_PEAK_LIMIT_PCM16
#define AUDIO_DSP_VOLUME_PERCENT_MAX             100U
#define AUDIO_DSP_PLAYBACK_FADE_MS              20U
#define AUDIO_DSP_PLAYBACK_FADE_SAMPLES         \
    ((AUDIO_DSP_SAMPLE_RATE_HZ * AUDIO_DSP_PLAYBACK_FADE_MS) / 1000U)
#define AUDIO_DSP_Q15_FULL_SCALE                32767U

typedef void (*audio_dsp_cooperative_yield_cb_t)(void *context);

typedef struct
{
    float real[AUDIO_DSP_NS_FFT_SIZE];
    float imag[AUDIO_DSP_NS_FFT_SIZE];
    float window[AUDIO_DSP_NS_FFT_SIZE];
    float overlap[AUDIO_DSP_NS_HOP_SIZE];
    float smoothed_power[AUDIO_DSP_NS_BIN_COUNT];
    float minimum_power[AUDIO_DSP_NS_BIN_COUNT];
    float noise_power[AUDIO_DSP_NS_BIN_COUNT];
    float previous_gain[AUDIO_DSP_NS_BIN_COUNT];
    uint16_t eligible_noise_frames[AUDIO_DSP_NS_BIN_COUNT];
    float fft_step_real[AUDIO_DSP_NS_FFT_STAGE_COUNT];
    float fft_step_imag_forward[AUDIO_DSP_NS_FFT_STAGE_COUNT];
    uint32_t frames_in_minimum_window;
    uint32_t noise_updates;
    bool power_initialized;
    bool noise_initialized;
    bool tables_initialized;
} audio_dsp_workspace_t;

typedef struct
{
    uint32_t processed_frames;
    uint32_t noise_updates;
    uint32_t processed_bin_frames;
    uint32_t floor_bin_frames;
    uint32_t speech_protected_bin_frames;
    uint32_t cooperative_yields;
    float average_gain;
    float minimum_gain;
    float maximum_gain;
} audio_dsp_ns_metrics_t;

typedef struct
{
    uint64_t absolute_sum;
    uint32_t peak;
    uint32_t sample_count;
    uint32_t soft_compressed_samples;
    uint32_t limited_samples;
} audio_dsp_playback_stats_t;

void audio_dsp_workspace_init(audio_dsp_workspace_t *workspace);
void audio_dsp_workspace_reset_recording(audio_dsp_workspace_t *workspace);
int32_t audio_dsp_convert_raw_slot_to_pcm24(int32_t raw_slot);

int32_t audio_dsp_calculate_dc_offset(
    const int32_t *samples,
    size_t sample_count);

int32_t audio_dsp_calculate_dc_offset_cooperative(
    const int32_t *samples,
    size_t sample_count,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context);

void audio_dsp_calculate_level(
    const int32_t *samples,
    size_t sample_count,
    uint32_t *average_absolute,
    uint32_t *peak);

void audio_dsp_calculate_level_cooperative(
    const int32_t *samples,
    size_t sample_count,
    uint32_t *average_absolute,
    uint32_t *peak,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context);

void audio_dsp_apply_speech_band_filter_in_place(
    int32_t *samples,
    size_t sample_count,
    int32_t dc_offset);

void audio_dsp_apply_speech_band_filter_in_place_cooperative(
    int32_t *samples,
    size_t sample_count,
    int32_t dc_offset,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context);

bool audio_dsp_apply_adaptive_ns_in_place(
    int32_t *samples,
    size_t sample_count,
    audio_dsp_workspace_t *workspace,
    audio_dsp_ns_metrics_t *metrics);

bool audio_dsp_apply_adaptive_ns_in_place_cooperative(
    int32_t *samples,
    size_t sample_count,
    audio_dsp_workspace_t *workspace,
    audio_dsp_ns_metrics_t *metrics,
    audio_dsp_cooperative_yield_cb_t yield_cb,
    void *yield_context);

int16_t audio_dsp_prepare_output_sample(
    int32_t sample_pcm24,
    size_t sample_index,
    size_t sample_count,
    bool *soft_compressed,
    bool *limited);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DSP_H */
