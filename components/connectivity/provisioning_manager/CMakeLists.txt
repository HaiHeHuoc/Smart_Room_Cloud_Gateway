idf_component_register(
    SRCS
        "provisioning_manager.c"
    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        esp_hw_support
        freertos
        log
        network_provisioning
)
