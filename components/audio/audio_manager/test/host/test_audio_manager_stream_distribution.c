#include <stdio.h>
#include <string.h>

#include "audio_manager_stream.h"
#include "audio_manager_stream_internal.h"

static unsigned s_failures = 0U;
static unsigned s_ptt_callbacks = 0U;
static uint32_t s_last_ptt_generation = 0U;
static int s_tap_arm_count = 0;
static int s_tap_disarm_count = 0;
static int s_tap_monitor_enabled_count = 0;

#define EXPECT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                      \
            ++s_failures;                                                        \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                        \
    } while (0)

void audio_manager_stream_tap_arm(void)
{
    ++s_tap_arm_count;
}

void audio_manager_stream_tap_disarm(void)
{
    ++s_tap_disarm_count;
}

void audio_manager_stream_tap_set_local_monitor_enabled(bool enabled)
{
    if (enabled) {
        ++s_tap_monitor_enabled_count;
    }
}

int32_t audio_manager_stream_convert_raw_slot_to_pcm24(int32_t raw_slot)
{
    return raw_slot;
}

static void ptt_callback(const audio_manager_stream_frame_t *frame,
                         void *user_context)
{
    (void)user_context;
    EXPECT_TRUE(frame != NULL);
    if (frame != NULL) {
        ++s_ptt_callbacks;
        s_last_ptt_generation = frame->stream_generation;
    }
}

static void publish_frame(int16_t sample)
{
    int16_t samples[AUDIO_MANAGER_STREAM_FRAME_SAMPLES] = {0};
    samples[0] = sample;
    EXPECT_TRUE(audio_manager_stream_publish_internal(
                    samples, AUDIO_MANAGER_STREAM_FRAME_SAMPLES) == ESP_OK);
}

int main(void)
{
    audio_manager_stream_local_monitor_frame_t frame = {0};
    audio_manager_stream_status_t status = {0};

    EXPECT_TRUE(audio_manager_stream_local_monitor_set_enabled(true) ==
                ESP_ERR_INVALID_STATE);
    EXPECT_TRUE(audio_manager_stream_local_monitor_register() == ESP_OK);
    EXPECT_TRUE(audio_manager_stream_local_monitor_register() ==
                ESP_ERR_INVALID_STATE);
    EXPECT_TRUE(audio_manager_stream_register_callback(ptt_callback, NULL) ==
                ESP_OK);
    EXPECT_TRUE(audio_manager_stream_local_monitor_set_enabled(true) == ESP_OK);
    EXPECT_TRUE(s_tap_monitor_enabled_count == 1);

    /* Fill the independent local queue. The fifth frame drops locally only. */
    publish_frame(1);
    publish_frame(2);
    publish_frame(3);
    publish_frame(4);
    publish_frame(5);
    EXPECT_TRUE(audio_manager_stream_get_status(&status) == ESP_OK);
    EXPECT_TRUE(status.local_monitor.frames_delivered == 4U);
    EXPECT_TRUE(status.local_monitor.frames_dropped_queue_full == 1U);
    EXPECT_TRUE(status.local_monitor.queue_peak_depth ==
                AUDIO_MANAGER_STREAM_LOCAL_MONITOR_QUEUE_LENGTH);

    EXPECT_TRUE(audio_manager_stream_local_monitor_receive(&frame, 0U) == ESP_OK);
    EXPECT_TRUE(frame.frame_sequence == 1U);
    EXPECT_TRUE(frame.samples[0] == 1);

    /* Sequence 5 was dropped. A later delivered frame exposes one bounded gap. */
    publish_frame(6);
    for (uint64_t expected = 2U; expected <= 4U; ++expected) {
        EXPECT_TRUE(audio_manager_stream_local_monitor_receive(&frame, 0U) == ESP_OK);
        EXPECT_TRUE(frame.frame_sequence == expected);
    }
    EXPECT_TRUE(audio_manager_stream_local_monitor_receive(&frame, 0U) == ESP_OK);
    EXPECT_TRUE(frame.frame_sequence == 6U);
    EXPECT_TRUE(audio_manager_stream_get_status(&status) == ESP_OK);
    EXPECT_TRUE(status.local_monitor.sequence_gap_count == 1U);

    /* PTT wins: a full/slow local queue cannot prevent the existing PTT path. */
    EXPECT_TRUE(audio_manager_stream_arm(77U) == ESP_OK);
    publish_frame(7);
    EXPECT_TRUE(s_ptt_callbacks == 1U);
    EXPECT_TRUE(s_last_ptt_generation == 77U);
    EXPECT_TRUE(audio_manager_stream_get_status(&status) == ESP_OK);
    EXPECT_TRUE(status.local_monitor.frames_suppressed_ptt == 1U);
    EXPECT_TRUE(audio_manager_stream_local_monitor_unregister() ==
                ESP_ERR_INVALID_STATE);
    EXPECT_TRUE(audio_manager_stream_disarm(77U) == ESP_OK);
    EXPECT_TRUE(s_tap_arm_count == 1);
    EXPECT_TRUE(s_tap_disarm_count == 1);

    EXPECT_TRUE(audio_manager_stream_local_monitor_set_enabled(false) == ESP_OK);
    EXPECT_TRUE(audio_manager_stream_local_monitor_receive(&frame, 0U) ==
                ESP_ERR_INVALID_STATE);
    EXPECT_TRUE(audio_manager_stream_local_monitor_unregister() == ESP_OK);
    EXPECT_TRUE(audio_manager_stream_publish_internal(NULL, 1U) ==
                ESP_ERR_INVALID_ARG);

    if (s_failures != 0U) {
        (void)fprintf(stderr, "%u stream distribution test(s) failed\n", s_failures);
        return 1;
    }
    (void)printf("audio manager PCM distribution: PASS\n");
    return 0;
}
