set(xiaozhi_foundation_sources
    "modules/mcp_cloud_sync/src/xiaozhi_mcp_cloud_sync.c"
    "modules/mcp_light_capabilities/src/xiaozhi_mcp_light_capabilities.c"
    "modules/mcp_light_state_query/src/xiaozhi_mcp_light_state_query.c"
    "modules/mcp_light_set_state/src/xiaozhi_mcp_light_set_state.c"
    "modules/mcp_sensor_query/src/xiaozhi_mcp_sensor_query.c"
    "modules/mcp_system_status/src/xiaozhi_mcp_system_status.c"
    "modules/session/src/xiaozhi_session.c"
    "modules/text_bridge/src/xiaozhi_text_bridge.c"
    "modules/websocket/src/xiaozhi_websocket_task_policy.c"
    "modules/websocket/src/xiaozhi_websocket_tx_timeout.c")

set(xiaozhi_foundation_private_requires
    app_log
    esp_event
    heap
    esp_timer
    freertos
    log
    cjson
    esp_xiaozhi
    esp_websocket_client
    mcp-c-sdk)

idf_component_register(
    SRCS
        ${xiaozhi_foundation_sources}

    INCLUDE_DIRS
        "include"

    PRIV_INCLUDE_DIRS
        "modules/mcp_cloud_sync/include"
        "modules/mcp_light_capabilities/include"
        "modules/mcp_light_state_query/include"
        "modules/mcp_light_set_state/include"
        "modules/mcp_sensor_query/include"
        "modules/mcp_system_status/include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        ${xiaozhi_foundation_private_requires}
)

# Phase 15: only the production session source is redirected through the
# semantic-text init bridge. The bridge calls the real esp_xiaozhi_chat_init()
# from its own translation unit, so transport behavior remains unchanged.
set_source_files_properties(
    modules/session/src/xiaozhi_session.c
    PROPERTIES
    COMPILE_DEFINITIONS
        "esp_xiaozhi_chat_init=xiaozhi_foundation_chat_init_bridge")

# esp_xiaozhi 0.1.2 calls the public WebSocket send APIs with portMAX_DELAY.
# Wrap only those public symbols at final link so project-owned finite-timeout
# shims protect text control frames and Opus frames without mutating the
# managed provider or using its private transport handles.
target_link_options(
    ${COMPONENT_LIB}
    INTERFACE
    "LINKER:--wrap=esp_websocket_client_init"
    "LINKER:--wrap=esp_websocket_client_send_text"
    "LINKER:--wrap=esp_websocket_client_send_bin"
    "LINKER:-u,__wrap_esp_websocket_client_init"
    "LINKER:-u,__wrap_esp_websocket_client_send_text"
    "LINKER:-u,__wrap_esp_websocket_client_send_bin")
