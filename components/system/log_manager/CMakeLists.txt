idf_component_register(
    SRCS
        "log_manager.c"
        "modules/buffer/src/log_buffer.c"
        "modules/console/src/log_console_color.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "modules/buffer/include"
    REQUIRES
        esp_common
        log
    PRIV_REQUIRES
        app_log
        freertos
        heap
        esp_timer
        esp_hw_support
        sd_card_manager
        time_manager
        common
)

# Keep console coloring local to log_manager's direct esp_log_write() call.
# The macro-renamed declaration comes from esp_log_write.h, while the wrapper
# implementation is compiled normally in the private console module.
set_source_files_properties(
    "log_manager.c"
    PROPERTIES COMPILE_DEFINITIONS "esp_log_write=log_manager_console_write"
)
