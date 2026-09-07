idf_component_register(
    SRCS
        "performance_monitor.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        app_log
        freertos
        esp_system
        esp_hw_support
        esp_psram
        spi_flash
        heap
        app_update
        bootloader_support
        log
)
