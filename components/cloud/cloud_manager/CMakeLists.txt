idf_component_register(
    SRCS
        "cloud_manager.c"
        "modules/push_latest_policy/src/cloud_push_latest_policy.c"
        "modules/telemetry_json/src/cloud_telemetry_json.c"

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "modules/push_latest_policy/include"
        "modules/telemetry_json/include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        app_log
        common
        firebase_auth
        esp_http_client
        mbedtls
        log
        freertos
        esp_timer
)
