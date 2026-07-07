# sd_card_manager Component Notes

## Purpose

`sd_card_manager` owns SD card bring-up over SDSPI.
It initializes the SD SPI bus, mounts the card as a FAT filesystem, and provides
simple read/write/list helpers for early hardware verification.

This component is currently a bring-up component, not yet a full storage service.

## What Is Done

- Initializes the SD card SPI bus using `SD_SPI_HOST` and SD GPIO settings from
  `board_config.h`.
- Mounts the SD card at `SD_MOUNT_POINT`.
- Uses ESP-IDF `esp_vfs_fat_sdspi_mount()` to attach SDSPI, initialize the card,
  and register FATFS with VFS.
- Tracks internal mount state with `s_sd_mounted`.
- Retries SD card initialization several times before returning failure.
- Provides `sd_card_manager_write_test_file()` to create `/sdcard/hello.txt`.
- Provides `sd_card_manager_read_test_file()` to read and log the same file.
- Provides `sd_card_manager_list_files()` to log one directory level.
- Provides `sd_card_manager_list_files_recursive()` to log a directory tree up
  to a caller-selected depth.

## How To Use

Initialize and mount the SD card:

```c
esp_err_t ret = sd_card_manager_init();
if (ret != ESP_OK) {
    return;
}
```

Check mount state:

```c
if (sd_card_manager_is_mounted()) {
    // Safe to use SD file APIs.
}
```

Run the current bring-up test:

```c
sd_card_manager_write_test_file();
sd_card_manager_read_test_file();
```

List files for debugging:

```c
sd_card_manager_list_files(NULL);
sd_card_manager_list_files_recursive(NULL, 2);
```

Passing `NULL` uses `SD_MOUNT_POINT`, currently `/sdcard`.

After mount succeeds, normal C file APIs can be used with the configured mount
point:

```c
FILE *file = fopen("/sdcard/example.txt", "w");
```

## Important Notes

- `format_if_mount_failed` is disabled. This protects existing SD card data
  during bring-up.
- `sd_card_manager_is_mounted()` reports internal software state only. It does
  not physically detect card removal.
- The current test file path is fixed and intended only for verification.
- File listing helpers print results to the log. They do not return a file list
  to the caller.
- Recursive listing uses `max_depth`; `0` means only the starting directory is
  scanned.
- `fclose()` is important after writes because it flushes buffered data to the
  filesystem.
- SD card speed is intentionally low for bring-up. Increase it only after the
  hardware wiring is stable.

## Future Attention

- Add a public unmount/deinit API when card removal or shutdown is needed.
- Add generic file read/write/list result APIs after the data format is known.
- Add better error reporting for common mount failures such as missing card,
  wrong wiring, unsupported format, or unstable power.
- Consider adding card-detect GPIO if the hardware supports it.
- Revisit SD clock speed after repeated write/read tests are stable.
