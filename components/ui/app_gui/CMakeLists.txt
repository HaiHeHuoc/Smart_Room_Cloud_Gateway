idf_component_register(
    SRCS
        "app_gui.c"
        "fonts/app_gui_xiaozhi_latin_12.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "modules/theme/include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        app_log
        common
        lvgl
        heap
        esp_timer
        time_manager
        ui_manager_lvgl
)
