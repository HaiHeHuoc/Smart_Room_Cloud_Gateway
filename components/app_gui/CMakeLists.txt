idf_component_register(
    SRCS "app_gui.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        lvgl
        heap
        ui_manager_lvgl
)
