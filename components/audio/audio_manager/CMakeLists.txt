idf_component_register(
    SRCS
        "audio_manager.c"
        "modules/arbitration/src/audio_manager_arbitration.c"
        "modules/arbitration/src/audio_manager_arbitration_policy.c"
        "modules/arbitration/src/audio_manager_playback_arbiter.c"
        "modules/arbitration/src/audio_manager_capture_arbiter.c"
        "modules/playback/src/audio_manager_named_playback.c"
        "modules/stream/src/audio_manager_pcm_stream_core.c"
        "modules/stream/src/audio_manager_stream.c"
        "modules/stream/src/audio_manager_stream_tap.c"
        "modules/test_support/src/audio_api_test_task.c"
        "modules/dsp/src/audio_dsp.c"
        "modules/dsp/src/audio_dsp_cooperative.c"
        "modules/wav/src/audio_wav.c"
        "modules/wav/src/audio_wav_prefetch.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "modules/dsp/include"
        "modules/stream/include"
        "modules/wav/include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        app_log
        common
        freertos
        log
        esp_driver_i2s
        esp_driver_gpio
        esp_timer
        heap
        esp_hw_support
        esp_psram
        sd_card_manager
)

# Phase 14 live-stream tap: only audio_manager.c redirects the raw-slot
# conversion symbol. The DSP module keeps the original implementation, while
# the wrapper copies selected recording samples into the bounded PCM16 stream.
set_source_files_properties(
    "audio_manager.c"
    PROPERTIES COMPILE_DEFINITIONS
    "audio_dsp_convert_raw_slot_to_pcm24=audio_manager_stream_convert_raw_slot_to_pcm24"
)
