idf_component_register(
    SRCS
        "provisioning_manager.c"
    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        esp_hw_support
        esp_wifi
        freertos
        log
        network_provisioning
)
