idf_component_register(
    SRCS "scene_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos
    PRIV_REQUIRES audio_manager light_manager voice_assistant
)
