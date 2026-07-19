idf_component_register(
    SRCS
        "config_manager.c"

    INCLUDE_DIRS
        "include"

    PRIV_REQUIRES
        freertos
        nvs_flash
)
