idf_component_register(
    SRCS "app_gui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        lvgl
        heap
        time_manager
        ui_manager_lvgl
)
