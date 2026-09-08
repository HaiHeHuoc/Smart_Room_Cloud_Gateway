idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common

    PRIV_REQUIRES
        app_log
        common
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        freertos
        log
)

# Scope the xTaskCreate/vTaskDelete PSRAM override to wifi_manager only.
# The reconnect worker uses RAM-only Wi-Fi driver operations and does not call
# NVS/flash/OTA/cache-disabled paths; persistent driver cleanup runs from the
# separate reset-coordinator task context.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
        "-includepsram_task_create.h"
)
