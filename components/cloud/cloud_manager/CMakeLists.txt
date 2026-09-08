idf_component_register(
    SRCS
        "cloud_manager.c"
        "modules/telemetry_json/src/cloud_telemetry_json.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "modules/telemetry_json/include"

    REQUIRES
        app_log
        esp_common

    PRIV_REQUIRES
        common
        firebase_auth
        esp_http_client
        mbedtls
        log
        freertos
        esp_timer
)
