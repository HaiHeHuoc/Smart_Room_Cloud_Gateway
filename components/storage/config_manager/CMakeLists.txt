idf_component_register(
    SRCS
        "config_manager.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_log
        esp_common

    PRIV_REQUIRES
        freertos
        log
        nvs_flash
)
