idf_component_register(
    SRCS
        "voice_assistant.c"
        "voice_assistant_audio_adapter.c"
        "voice_assistant_ui_model.c"
        "voice_assistant_ui_gui_adapter.c"
        "voice_assistant_ptt.c"
        "voice_assistant_ptt_gpio.c"
        "voice_assistant_opus.c"
        "voice_assistant_uplink.c"
        "voice_assistant_downlink.c"
        "voice_assistant_audio_arbitration_bridge.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        esp_driver_gpio
        xiaozhi_foundation
        audio_manager
        app_gui

    PRIV_REQUIRES
        esp_audio_codec
        esp_timer
        freertos
        log
        heap
)

# Phase 16 migration: Xiaozhi uplink is redirected through the capture bridge.
# Phase 16.1 downlink calls its streaming bridge explicitly so it cannot bypass
# playback arbitration or take ownership of I2S/DMA.
set_source_files_properties(
    "voice_assistant_uplink.c"
    PROPERTIES COMPILE_DEFINITIONS
    "audio_manager_start_recording=phase16_xiaozhi_start_recording;audio_manager_stop_recording=phase16_xiaozhi_stop_recording"
)
