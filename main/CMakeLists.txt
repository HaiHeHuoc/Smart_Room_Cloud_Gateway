idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
    REQUIRES
        common
        display_driver
        ui_manager_lvgl
        )
