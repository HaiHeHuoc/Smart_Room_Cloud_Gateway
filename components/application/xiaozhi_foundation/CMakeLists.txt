set(xiaozhi_foundation_sources
    "xiaozhi_foundation.c"
    "xiaozhi_session.c")

set(xiaozhi_foundation_private_requires
    esp_event
    heap
    esp_timer
    freertos
    log
    esp_xiaozhi
    mcp-c-sdk)

if(CONFIG_XIAOZHI_FOUNDATION_P2F_SD_FIXTURE)
    list(APPEND xiaozhi_foundation_sources
        "p2f_sd_fixture_loader.c"
        "p2f_sd_fixture_buffer.S")
    list(APPEND xiaozhi_foundation_private_requires
        sd_card_manager)
endif()

idf_component_register(
    SRCS
        ${xiaozhi_foundation_sources}

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_common

    PRIV_REQUIRES
        ${xiaozhi_foundation_private_requires}
)

if(CONFIG_XIAOZHI_FOUNDATION_P2F_SD_FIXTURE)
    # The validation-only wrapper loads the SD fixture immediately before the
    # existing public transport-validation request reaches xiaozhi_foundation.
    target_link_options(
        ${COMPONENT_LIB}
        INTERFACE
        "LINKER:--wrap=xiaozhi_foundation_request_transport_validation")
elseif(CONFIG_XIAOZHI_FOUNDATION_P2F_EMBED_FIXTURE)
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
