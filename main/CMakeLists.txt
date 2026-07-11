idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
    REQUIRES
        common
        display_driver
        ui_manager_lvgl
        sd_card_manager
        lvgl_sd_fs
        lvgl_image_handler
        performance_monitor
        )
