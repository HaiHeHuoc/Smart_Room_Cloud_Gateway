idf_component_register(
    SRCS
        "xiaozhi_foundation.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        log
        esp_xiaozhi
)
