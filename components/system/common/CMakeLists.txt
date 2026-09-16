idf_component_register(
    SRCS
        "voice_recording_critical.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        esp_driver_gpio
        esp_driver_spi
    PRIV_REQUIRES
        freertos
)
