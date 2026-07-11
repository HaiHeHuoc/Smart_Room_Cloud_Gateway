set(LVGL_IMAGE_HANDLER_SRCS "lvgl_image_handler.c")

if(CONFIG_LV_USE_GIF)
    list(APPEND LVGL_IMAGE_HANDLER_SRCS
        "lvgl_image_handler_animated_gif.c")
endif()

idf_component_register(
    SRCS ${LVGL_IMAGE_HANDLER_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        common
        display_driver
        freertos
        libpng
        lvgl
        sd_card_manager
        lvgl_sd_fs
)
