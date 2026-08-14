# sd_card_manager Component Notes

## Purpose

`sd_card_manager` is the sole owner of the board's SDSPI bus, the FAT VFS
mount, and SD recovery lifecycle. It mounts at `SD_MOUNT_POINT` (`/sdcard`),
but a missing or slow card never stops the rest of the firmware from booting.

The component does not own application files, LVGL, WAV parsing, or audio I2S.
Consumers use normal VFS/stdio only through the managed lease contract below.

## Lifecycle

`sd_card_manager_init()` prepares only in-memory state; it does not touch SPI
or the card. `sd_card_manager_start()` creates one lifetime recovery task:

```text
app_main
  -> initialize LVGL and app_gui
  -> present the built-in BOOT / "Starting..." screen
  -> sd_card_manager_start()

sd_recovery task (priority 3, 4096-byte stack)
  -> 1 s cold-card settle delay
  -> mount attempt every 2 s for up to 90 s
  -> READY on success
  -> UNAVAILABLE after the initial deadline, while retrying every 2 s
```

The 90-second value is a user-visible initial-recovery deadline, not one
90-second blocking call. Every driver/VFS call still has its own ESP-IDF
timeout. Network, sensor, GUI, and direct record/DSP/playback audio continue
while the card is unavailable.

When a managed consumer reports a confirmed VFS I/O error, new opens are
blocked, existing leases drain, then the recovery task safely unmounts the VFS,
releases the SPI bus, and resumes its retry loop. The recovery task never calls
LVGL or consumer callbacks.

## Status API

`sd_card_manager_get_status()` copies a diagnostic snapshot without doing card
I/O:

```c
sd_card_manager_status_t status = {0};
if (sd_card_manager_get_status(&status) == ESP_OK) {
    // inspect state, last_error, mount attempts, I/O errors, and active leases
}
```

States are `INITIALIZING`, `MOUNTING`, `RETRY_WAIT`, `READY`, `RECOVERING`,
and `UNAVAILABLE` (plus `UNINITIALIZED`). `initial_recovery_timed_out` remains
true if the first successful mount occurred only after the initial deadline.

`sd_card_manager_is_mounted()` is only a fast logical availability hint. It is
not a reservation and is not a physical card-detect result.

## Managed VFS Lease Contract

Before opening an SD `FILE *` or `DIR *`, a consumer must acquire one lease and
release it only after the handle is closed:

```c
if (sd_card_manager_acquire() == ESP_OK) {
    FILE *file = fopen("/sdcard/example.bin", "rb");

    /* Use file, then close it before releasing the lease. */
    if (file != NULL) {
        fclose(file);
    }

    sd_card_manager_release();
}
```

`lvgl_sd_fs` wraps each LVGL file handle with this lease. The private
`audio_wav` stream does the same. This blocks an unmount race when a card fails
during an image decode or WAV stream. A consumer must call
`sd_card_manager_report_io_error()` only for a real VFS/media failure such as
`ferror()`, `fseek()`, or `fclose()` failure—not normal EOF or a missing path.
For a failed `fopen()`/`opendir()`/`stat()` call, capture `errno` immediately
and use `sd_card_manager_is_vfs_media_error()` first; it recognizes the
ESP-IDF FAT VFS media mappings `EIO`, `ENODEV`, `ENXIO`, and `ETIMEDOUT`, while
leaving normal `ENOENT` asset errors alone.

The public read/write/list bring-up helpers also hold an internal lease for the
complete handle lifetime.

## LVGL Integration

`lvgl_sd_fs_register()` may be registered once after `lv_init()`, before SD is
mounted. Its `ready_cb` dynamically follows the manager state, so no LVGL
driver re-registration is needed after a mount or recovery:

```c
ESP_ERROR_CHECK(sd_card_manager_init());
ESP_ERROR_CHECK(lvgl_sd_fs_register());
/* app_gui starts and presents BOOT here. */
ESP_ERROR_CHECK(sd_card_manager_start());
```

`S:/...` remains LVGL-only. Audio uses the canonical VFS path `/sdcard/...`.

## Bring-up Helpers

The following helpers remain for manual hardware checks after status is
`READY`:

```c
sd_card_manager_write_test_file();
sd_card_manager_read_test_file();
sd_card_manager_list_files(NULL);
sd_card_manager_list_files_recursive(NULL, 2U);
```

Automatic formatting is disabled. A mount failure never formats or erases card
data.

## Hardware Limits

- This board configuration has no `SD_GPIO_CD` card-detect input and no
  `SD_PWR_EN` control. Firmware cannot truthfully guarantee idle physical
  removal detection or power-cycle a card.
- The five-second idle `sdmmc_get_status()` probe is best effort only. It runs
  only when there are no managed file leases, so it cannot race VFS I/O.
- Runtime recovery depends on all SD VFS consumers following the lease
  contract. Unmanaged direct `fopen("/sdcard/..." )` code is unsupported.
- There is no public stop/deinit API because this component's recovery task is
  designed for the firmware lifetime. A coordinated application shutdown is a
  future lifecycle feature.
- The current clock is the board-configured conservative 1 MHz. Increase it
  only after repeated cold-boot and read/write hardware acceptance.

## Hardware Acceptance Pending

The asynchronous recovery behavior is not hardware-accepted until it is
observed on the target. Capture serial evidence for these cases before raising
the SD clock or depending on WAV assets in production:

1. Start with a cold or absent card: confirm BOOT/UI, Wi-Fi, sensor, and direct
   record/DSP/playback audio continue while mount retries occur every two
   seconds for the full 90-second initial window.
2. Insert or make the card electrically ready after initial timeout: confirm a
   later background retry reaches `READY` without rebooting the application.
3. Open an LVGL asset or WAV stream, then induce a real SD I/O failure: confirm
   the file closes, the final lease drains, recovery unmounts once, and a later
   successful mount permits a new open.
4. Remove a card while it is idle: because there is no card-detect pin, expect
   detection only from the next five-second no-lease health probe or a later
   VFS operation; firmware cannot prove removal instantly.
