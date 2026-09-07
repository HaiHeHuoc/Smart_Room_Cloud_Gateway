idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_log
        esp_common

    PRIV_REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        freertos
        log
)
