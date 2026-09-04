idf_component_register(
    SRCS
        "main.c"
        "phase14_voice_composition.c"
        "phase16_auto_hil_test.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        common
        display_driver
        ui_manager_lvgl
        sd_card_manager
        lvgl_sd_fs
        lvgl_image_handler
        performance_monitor
        time_manager
        wifi_manager
        app_gui
        sensor_manager
        cloud_manager
        firebase_auth
        config_manager
        provisioning_manager
        app_network_coordinator
        xiaozhi_foundation
        voice_assistant
        button_manager
        app_reset_coordinator
        audio_manager

    PRIV_REQUIRES
        nvs_flash
        esp_netif
        esp_event
)

# Phase 14 composition is scoped to main.c only: keep audio_manager's public
# implementation unchanged while routing the application registration/start
# calls through a wrapper that fans out status and starts the voice stack only
# after the audio task has reached its normal startup boundary.
set_property(
    SOURCE "main.c"
    APPEND PROPERTY COMPILE_DEFINITIONS
        audio_manager_register_status_callback=app_phase14_audio_manager_register_status_callback
        audio_manager_start=app_phase14_audio_manager_start
)
