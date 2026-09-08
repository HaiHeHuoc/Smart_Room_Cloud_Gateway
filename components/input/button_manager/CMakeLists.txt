idf_component_register(
    SRCS "button_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_log
        esp_common
        esp_driver_gpio

    PRIV_REQUIRES
        freertos
        heap
        log
)
