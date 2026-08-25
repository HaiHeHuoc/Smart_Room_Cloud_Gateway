idf_component_register(
    SRCS
        "audio_manager.c"
        "audio_manager_arbitration.c"
        "audio_manager_playback_arbiter.c"
        "audio_manager_capture_arbiter.c"
        "audio_manager_stream.c"
        "audio_manager_stream_tap.c"
        "audio_api_test_task.c"
        "audio_dsp.c"
        "audio_dsp_cooperative.c"
        "audio_wav.c"
        "audio_wav_prefetch.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        esp_common
        common
    PRIV_REQUIRES
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
# conversion symbol. audio_dsp.c keeps the original implementation, while the
# wrapper copies selected recording samples into the bounded PCM16 stream.
set_source_files_properties(
    "audio_manager.c"
    PROPERTIES COMPILE_DEFINITIONS
    "audio_dsp_convert_raw_slot_to_pcm24=audio_manager_stream_convert_raw_slot_to_pcm24"
)
