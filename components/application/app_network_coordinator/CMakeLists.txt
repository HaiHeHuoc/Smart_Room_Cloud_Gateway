idf_component_register(
    SRCS
        "app_network_coordinator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        freertos
        log
        app_gui
        config_manager
        provisioning_manager
        wifi_manager
        xiaozhi_foundation
)
