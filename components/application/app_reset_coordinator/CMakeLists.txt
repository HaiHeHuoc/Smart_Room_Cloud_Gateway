idf_component_register(
    SRCS
        "app_reset_coordinator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        app_log
        freertos
        log
        log_manager
        app_network_coordinator
        config_manager
        esp_system
        wifi_manager
        app_gui
)
