idf_component_register(
    SRCS
        "xiaozhi_foundation.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        esp_event
        esp_timer
        freertos
        log
        esp_xiaozhi
        mcp-c-sdk
)

if(CONFIG_XIAOZHI_FOUNDATION_P2F_EMBED_FIXTURE)
    set(xiaozhi_foundation_p2f_fixture
        "${CMAKE_CURRENT_LIST_DIR}/test_assets/p2f_fixture.bin")

    if(NOT EXISTS "${xiaozhi_foundation_p2f_fixture}")
        message(FATAL_ERROR
            "P2-F fixture is enabled but missing: ${xiaozhi_foundation_p2f_fixture}. "
            "See test_assets/README.md; do not use arbitrary PCM or an Ogg container.")
    endif()

    target_add_binary_data(
        ${COMPONENT_LIB}
        "${xiaozhi_foundation_p2f_fixture}"
        BINARY
        RENAME_TO "xiaozhi_p2f_fixture")
endif()
