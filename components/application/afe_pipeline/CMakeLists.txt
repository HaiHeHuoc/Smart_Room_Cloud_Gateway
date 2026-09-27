idf_component_register(
    SRCS "afe_pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_manager esp_common
    PRIV_REQUIRES
        app_log
        esp-sr
        esp_timer
        freertos
        performance_monitor
)
