idf_component_register(
    SRCS
        "test_config_manager.c"

    PRIV_REQUIRES
        config_manager
        freertos
        log
        nvs_flash
        unity
)
