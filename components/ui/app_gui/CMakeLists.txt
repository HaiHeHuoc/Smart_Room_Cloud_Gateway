idf_component_register(
    SRCS
        "app_gui.c"
        "fonts/app_gui_xiaozhi_latin_12.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        lvgl
        heap
        esp_timer
        time_manager
        ui_manager_lvgl
)
