idf_component_register(
    SRCS
        "voice_assistant.c"
        "voice_assistant_bootstrap.c"
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
        esp_common
        esp_driver_gpio
        audio_manager

    PRIV_REQUIRES
        app_log
        app_gui
        app_network_coordinator
        common
        esp_audio_codec
        esp_timer
        freertos
        log
        heap
        xiaozhi_foundation
)
