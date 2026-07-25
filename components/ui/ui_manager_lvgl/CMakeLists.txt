idf_component_register(
    SRCS "ui_manager_lvgl.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        display_driver
        lvgl
        esp_lcd
        esp_timer
        heap
)
