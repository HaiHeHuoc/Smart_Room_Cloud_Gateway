idf_component_register(
    SRCS "sd_card_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        app_log
        common
        fatfs
        sdmmc
        esp_driver_gpio
        esp_driver_spi
        freertos
)

# Scope the xTaskCreate/vTaskDelete PSRAM override to sd_card_manager only.
# The recovery worker owns SDSPI/FATFS mount, health, unmount, and retry work;
# it does not execute internal-flash/NVS/OTA/cache-disabled operations.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
        "-includepsram_task_create.h"
)
