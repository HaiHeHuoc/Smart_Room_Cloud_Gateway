# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/application"
    "${CMAKE_CURRENT_LIST_DIR}/components/audio"
    "${CMAKE_CURRENT_LIST_DIR}/components/cloud"
    "${CMAKE_CURRENT_LIST_DIR}/components/connectivity"
    "${CMAKE_CURRENT_LIST_DIR}/components/display"
    "${CMAKE_CURRENT_LIST_DIR}/components/sensing"
    "${CMAKE_CURRENT_LIST_DIR}/components/storage"
    "${CMAKE_CURRENT_LIST_DIR}/components/system"
    "${CMAKE_CURRENT_LIST_DIR}/components/ui"
    "${CMAKE_CURRENT_LIST_DIR}/components/input"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Smart_Room_Cloud_Gateway)

# ESP-IDF v6.0.1 leaks cross-signed certificate name allocations after each
# TLS handshake. Keep cross-signed verification enabled for Firebase and apply
# the exact upstream lifetime fix to the pinned SDK source at configure time.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/esp_idf_6_0_1_crt_bundle_fix.cmake")
