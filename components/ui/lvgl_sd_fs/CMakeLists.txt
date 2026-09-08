idf_component_register(
    SRCS "lvgl_sd_fs.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        app_log
        common
        lvgl
        sd_card_manager
)
