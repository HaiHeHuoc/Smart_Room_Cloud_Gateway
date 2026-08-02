idf_component_register(
    SRCS "sensor_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        common
        dht
        esp_timer
        freertos
        heap
        sensor_DHT22
)

# Scope the xTaskCreate -> xTaskCreateWithCaps override to this component only.
# The DHT22 path remains task-context only and performs no flash/NVS work.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
        "-includepsram_task_create.h"
)
