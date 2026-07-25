# config_manager Test Application

## Purpose

This ESP-IDF application is the isolated destructive regression suite for the
entire `config_manager` component. Its name and layout are component-oriented
so future storage tests are not tied to one historical Phase 5 sub-phase.

The test app accesses raw NVS only to inject missing, malformed, legacy,
unsupported, and interrupted states. Production firmware exposes no
fault-injection API.

## Warning

The application calls `nvs_flash_erase()` at boot and destroys configuration
stored in the default NVS partition. Do not flash it to a device whose saved
configuration must be retained.

## Structure

```text
Test/config_manager/
|-- .gitignore
|-- CMakeLists.txt
|-- README.md
|-- sdkconfig.defaults
`-- main/
    |-- CMakeLists.txt
    `-- test_config_manager.c
```

Generated `build/`, `sdkconfig`, and `sdkconfig.old` files are ignored.

## Coverage

The suite covers:

- Calls before initialization and repeated initialization.
- Secured and open Wi-Fi networks.
- Missing, incomplete, malformed, and oversized NVS values.
- Legacy schema detection and explicit migration.
- Unsupported and interrupted Wi-Fi update states.
- Device identity save/load/clear and corruption handling.
- Typed custom data mismatch and output clearing.
- Wi-Fi clear and factory-reset ownership boundaries.
- Repeated save/load/clear/reset cycles.
- Concurrent reader/writer serialization.

## Build

```powershell
cd Test/config_manager
idf.py set-target esp32s3
idf.py build
```

## Flash and Monitor

```powershell
idf.py -p <PORT> flash monitor
```

Expected current result:

```text
38 Tests 0 Failures 0 Ignored
```

The previous 37-test version passed on ESP32-S3 hardware. The new
interrupted-write case and fail-safe publication sequence compile and link
successfully but require a fresh hardware run.
