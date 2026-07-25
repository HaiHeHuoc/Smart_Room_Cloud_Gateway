idf_component_register(
    SRCS
        "firebase_auth.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        esp_http_client
        esp_timer
        mbedtls
        freertos
        log
        cjson
)
