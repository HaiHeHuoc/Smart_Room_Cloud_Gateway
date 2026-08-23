idf_component_register(
    SRCS
        "xiaozhi_foundation.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        esp_event

    PRIV_REQUIRES
        freertos
        log
        esp_xiaozhi
)
