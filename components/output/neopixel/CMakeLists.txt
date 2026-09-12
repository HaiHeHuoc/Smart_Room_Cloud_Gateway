idf_component_register(
    SRCS "neopixel.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES led_strip freertos
)
