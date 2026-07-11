idf_component_register(
    SRCS "lvgl_image_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        display_driver
        lvgl
        sd_card_manager
        lvgl_sd_fs
)
