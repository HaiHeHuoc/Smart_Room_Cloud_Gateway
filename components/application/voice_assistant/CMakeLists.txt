idf_component_register(
    SRCS
        "voice_assistant.c"
        "voice_assistant_audio_adapter.c"
        "voice_assistant_ptt.c"
        "voice_assistant_uplink.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        xiaozhi_foundation
        audio_manager

    PRIV_REQUIRES
        freertos
        log
)
