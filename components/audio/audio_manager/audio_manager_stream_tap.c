#include "audio_manager_stream_internal.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_dsp.h"
#include "audio_manager_stream.h"

/* Keep these coupled to audio_manager.c's proven capture geometry. */
#define STREAM_TAP_SLOT_DETECT_BLOCKS   20U
#define STREAM_TAP_FRAMES_PER_BLOCK     256U
#define STREAM_TAP_SLOT_COUNT           2U
#define STREAM_TAP_DETECT_CONVERSIONS \
    (STREAM_TAP_SLOT_DETECT_BLOCKS * STREAM_TAP_FRAMES_PER_BLOCK * \
     STREAM_TAP_SLOT_COUNT)

_Static_assert(
    AUDIO_MANAGER_STREAM_FRAME_SAMPLES == STREAM_TAP_FRAMES_PER_BLOCK,
    "live stream frame must match audio-manager RX block size");

static atomic_bool s_enabled = false;
static atomic_uint_fast32_t s_discard_remaining = 0U;
static int16_t s_frame[AUDIO_MANAGER_STREAM_FRAME_SAMPLES];
static size_t s_frame_fill = 0U;

void audio_manager_stream_tap_arm(void)
{
    s_frame_fill = 0U;
    atomic_store_explicit(
        &s_discard_remaining,
        STREAM_TAP_DETECT_CONVERSIONS,
        memory_order_release);
    atomic_store_explicit(&s_enabled, true, memory_order_release);
}

void audio_manager_stream_tap_disarm(void)
{
    atomic_store_explicit(&s_enabled, false, memory_order_release);
    s_frame_fill = 0U;
    atomic_store_explicit(&s_discard_remaining, 0U, memory_order_release);
}

static int16_t stream_tap_pcm24_to_pcm16(int32_t pcm24)
{
    int32_t pcm16 = pcm24 / (int32_t)AUDIO_DSP_PCM24_SCALE_FACTOR;
    if (pcm16 > INT16_MAX) {
        pcm16 = INT16_MAX;
    } else if (pcm16 < INT16_MIN) {
        pcm16 = INT16_MIN;
    }
    return (int16_t)pcm16;
}

int32_t audio_manager_stream_convert_raw_slot_to_pcm24(int32_t raw_slot)
{
    const int32_t pcm24 = audio_dsp_convert_raw_slot_to_pcm24(raw_slot);

    if (!atomic_load_explicit(&s_enabled, memory_order_acquire)) {
        return pcm24;
    }

    uint_fast32_t discard = atomic_load_explicit(
        &s_discard_remaining,
        memory_order_acquire);
    if (discard > 0U) {
        (void)atomic_fetch_sub_explicit(
            &s_discard_remaining,
            1U,
            memory_order_acq_rel);
        return pcm24;
    }

    s_frame[s_frame_fill++] = stream_tap_pcm24_to_pcm16(pcm24);
    if (s_frame_fill == AUDIO_MANAGER_STREAM_FRAME_SAMPLES) {
        (void)audio_manager_stream_publish_internal(
            s_frame,
            AUDIO_MANAGER_STREAM_FRAME_SAMPLES);
        s_frame_fill = 0U;
    }

    return pcm24;
}
