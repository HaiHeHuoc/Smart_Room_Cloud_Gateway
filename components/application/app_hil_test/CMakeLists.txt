set(app_hil_test_sources
    "app_hil_test.c")

set(app_hil_test_private_requires
    app_log
    audio_manager)

if(CONFIG_APP_PHASE16_AUTO_HIL_TEST)
    list(APPEND app_hil_test_sources
        "modules/phase16/src/app_hil_phase16_auto.c")
    list(APPEND app_hil_test_private_requires
        app_gui
        cloud_manager
        esp_timer
        freertos
        heap
        sd_card_manager
        sensor_manager
        voice_assistant
        xiaozhi_foundation)
endif()

idf_component_register(
    SRCS
        ${app_hil_test_sources}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "modules/phase16/include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        ${app_hil_test_private_requires}
)
