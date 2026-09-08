idf_component_register(
    SRCS
        "ui_manager_lvgl.c"
        "modules/allocator/src/lvgl_psram_allocator.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common
        display_driver

    PRIV_REQUIRES
        app_log
        common
        lvgl
        esp_lcd
        esp_timer
        heap
        esp_hw_support

    WHOLE_ARCHIVE
)
