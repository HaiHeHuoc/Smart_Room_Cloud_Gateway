idf_component_register(
    SRCS "app_gui.c" "app_gui_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        lvgl
        heap
        ui_manager_lvgl
)
