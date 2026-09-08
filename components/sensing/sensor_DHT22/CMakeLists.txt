idf_component_register(
    SRCS "sensor_DHT22.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        esp_driver_gpio
    PRIV_REQUIRES
        app_log
        dht
        freertos
        log
)
