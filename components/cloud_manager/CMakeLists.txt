idf_component_register(
    SRCS "cloud_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        wifi_manager
        sensor_manager
        esp_http_client
        mbedtls
        log
        freertos
        esp_timer
)
