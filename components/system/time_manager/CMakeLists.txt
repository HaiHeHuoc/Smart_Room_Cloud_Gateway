idf_component_register(
    SRCS
        "time_manager.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        esp_netif
        freertos
        log
)
