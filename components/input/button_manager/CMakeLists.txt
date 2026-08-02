idf_component_register(
    SRCS "button_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        esp_driver_gpio

    PRIV_REQUIRES
        common
        freertos
        log
)

# Scope the xTaskCreate -> xTaskCreateWithCaps override to this component only.
# button_manager does not perform flash/NVS/cache-off work in its task path.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
        "-includepsram_task_create.h"
)
