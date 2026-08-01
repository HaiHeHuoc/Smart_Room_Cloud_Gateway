idf_component_register(
    SRCS
        "app_reset_coordinator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        freertos
        log
        app_network_coordinator
        config_manager
        esp_system
        wifi_manager
        app_gui
)
