idf_component_register(
    SRCS "display_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        esp_lcd
        esp_driver_gpio
        esp_driver_spi
        waveshare__esp_lcd_st7735
)
