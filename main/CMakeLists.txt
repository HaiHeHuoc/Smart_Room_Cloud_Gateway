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
        wifi_manager
        app_gui
        sensor_DHT22
        sensor_manager
        cloud_manager

    PRIV_REQUIRES
        nvs_flash
        esp_netif
        esp_event
        )
