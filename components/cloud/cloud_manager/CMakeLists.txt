idf_component_register(
    SRCS
        "cloud_manager.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        common
        firebase_auth
        esp_http_client
        mbedtls
        log
        freertos
        esp_timer
)
