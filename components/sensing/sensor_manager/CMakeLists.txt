idf_component_register(
    SRCS "sensor_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        common
        dht
        esp_timer
        sensor_DHT22
)
