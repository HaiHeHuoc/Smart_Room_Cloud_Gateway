idf_component_register(
    SRCS "log_manager.c" "log_buffer.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES freertos heap esp_timer esp_hw_support sd_card_manager time_manager common
    REQUIRES log app_log
)
