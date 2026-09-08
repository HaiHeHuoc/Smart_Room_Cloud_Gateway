idf_component_register(
    SRCS
        "voice_assistant.c"
        "modules/audio/src/voice_assistant_audio_adapter.c"
        "modules/audio/src/voice_assistant_audio_arbitration_bridge.c"
        "modules/ui/src/voice_assistant_ui_model.c"
        "modules/ui/src/voice_assistant_ui_gui_adapter.c"
        "modules/ptt/src/voice_assistant_ptt.c"
        "modules/ptt/src/voice_assistant_ptt_gpio.c"
        "modules/codec/src/voice_assistant_opus.c"
        "modules/uplink/src/voice_assistant_uplink.c"
        "modules/downlink/src/voice_assistant_downlink.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "modules/codec/include"

    REQUIRES
        app_log
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
    "modules/uplink/src/voice_assistant_uplink.c"
    PROPERTIES COMPILE_DEFINITIONS
    "audio_manager_start_recording=phase16_xiaozhi_start_recording;audio_manager_stop_recording=phase16_xiaozhi_stop_recording"
)
