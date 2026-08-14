# lvgl_sd_fs Component Notes

## Purpose

`lvgl_sd_fs` adapts LVGL's `S:` filesystem drive to the `/sdcard` VFS mounted
by `sd_card_manager`.

```text
S:/images/logo.png
    -> lvgl_sd_fs
    -> /sdcard/images/logo.png
```

It does not own SDSPI, FATFS, mount/unmount, retry timing, or UI routing.

## Offline-First Registration

The driver is registered once after LVGL initialization, before the SD card is
required to be available:

```c
ESP_ERROR_CHECK(ui_manager_lvgl_init(&display_handle));
ESP_ERROR_CHECK(sd_card_manager_init());
ESP_ERROR_CHECK(lvgl_sd_fs_register());
ESP_ERROR_CHECK(app_gui_init());
ESP_ERROR_CHECK(app_gui_start_ui_task());
ESP_ERROR_CHECK(sd_card_manager_start());
```

`ready_cb` follows `sd_card_manager_is_mounted()`. Therefore `S:` is not ready
while SD recovery is mounting/retrying, becomes ready after a successful mount,
and becomes unavailable again during safe recovery. Do not register a second
LVGL driver after reconnect, and do not call `lvgl_sd_fs_register()` from the
SD recovery task.

## File-Lifetime Safety

Each successful LVGL `open_cb` creates a small private opaque wrapper:

```text
LVGL FILE request
    -> sd_card_manager_acquire()
    -> fopen("/sdcard/...")
    -> wrapper { FILE *, lease }
    -> LVGL read / seek / tell
    -> fclose()
    -> sd_card_manager_release()
```

This keeps a managed SD lease from opening to closing the `FILE *`. If a real
read/seek/tell/close error occurs, the adapter reports it to `sd_card_manager`.
The manager then rejects new opens, waits for open handles to close, and
performs unmount/retry in its own task. The callbacks never mount, unmount, or
call LVGL outside LVGL's normal file-operation flow.

Normal EOF is not an SD failure. A missing asset is also not treated as a card
fault. A failed open whose errno is classified by
`sd_card_manager_is_vfs_media_error()` is reported as a media failure, while
normal missing-path errors are not.

## Public API

```c
esp_err_t lvgl_sd_fs_register(void);
bool lvgl_sd_fs_is_registered(void);
bool lvgl_sd_fs_is_ready(void);
```

`lvgl_sd_fs_is_registered()` reports only driver registration.
`lvgl_sd_fs_is_ready()` additionally requires the SD manager to accept new VFS
leases.

## Current Support

- Drive letter `S`.
- File open, close, read, seek, and tell callbacks.
- Standard C stdio through ESP-IDF VFS.
- Dynamic offline/ready behavior across SD recovery.
- LVGL cache remains disabled (`cache_size = 0`) to keep RAM behavior simple.

Write mode is mapped in `open_cb`, but no LVGL write/remove/rename/directory
callbacks are implemented. The active project assets are read-oriented.

## Limits

- The board has no card-detect GPIO, so a card removal while idle is not a
  guaranteed physical detection event.
- The manager's idle health probe is best effort; it runs only when this
  adapter and other managed consumers have no open lease.
- Asset code must close every LVGL file handle. A forgotten handle deliberately
  delays recovery rather than risking VFS use-after-free.
