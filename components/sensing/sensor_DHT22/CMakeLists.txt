idf_component_register(
    SRCS "sensor_DHT22.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        common
        dht
)
