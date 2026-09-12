idf_component_register(
    SRCS
        "light_manager.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        esp_driver_gpio

    PRIV_REQUIRES
        freertos
        log
        neopixel
)
