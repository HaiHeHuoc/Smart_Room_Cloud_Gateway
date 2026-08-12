idf_component_register(
    SRCS "sd_card_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        fatfs
        sdmmc
        esp_driver_gpio
        esp_driver_spi
    PRIV_REQUIRES
        freertos
)
