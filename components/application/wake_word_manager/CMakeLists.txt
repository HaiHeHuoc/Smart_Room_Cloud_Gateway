idf_component_register(
    SRCS "wake_word_manager.c" "modules/runtime/src/wake_word_runtime_policy.c"
    INCLUDE_DIRS "include" "modules/runtime/include"
    REQUIRES audio_manager esp_common
    PRIV_REQUIRES app_log common esp-sr esp_timer freertos
)
