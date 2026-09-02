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

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        esp_driver_gpio
        xiaozhi_foundation
        audio_manager
        sd_card_manager
        app_gui

    PRIV_REQUIRES
        esp_audio_codec
        esp_timer
        freertos
        log
        heap
)
