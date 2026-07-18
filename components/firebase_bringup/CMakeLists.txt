idf_component_register(
    SRCS "firebase_bringup.c"

    INCLUDE_DIRS
        "include"

    PRIV_REQUIRES
        esp_http_client
        mbedtls
        log
)