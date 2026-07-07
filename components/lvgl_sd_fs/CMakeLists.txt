idf_component_register(
    SRCS "lvgl_sd_fs.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        display_driver
        lvgl
        sd_card_manager
)
