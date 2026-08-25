idf_component_register(
    SRCS
        "voice_assistant.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        xiaozhi_foundation

    PRIV_REQUIRES
        freertos
        log
)
