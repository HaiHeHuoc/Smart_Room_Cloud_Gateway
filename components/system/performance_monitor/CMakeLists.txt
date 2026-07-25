idf_component_register(
    SRCS
        "performance_monitor.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        freertos
        esp_system
        heap
        app_update
        bootloader_support
        log
)