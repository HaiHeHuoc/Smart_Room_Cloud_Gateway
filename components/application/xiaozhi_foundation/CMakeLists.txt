idf_component_register(
    SRCS
        "xiaozhi_foundation.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        esp_event
        freertos
        log
        esp_xiaozhi
        mcp-c-sdk
)
