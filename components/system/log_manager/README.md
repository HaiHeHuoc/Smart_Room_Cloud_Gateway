# Application log manager

Centralized, best-effort application logging for the configured ESP-IDF 6.0.1
ESP32-S3 project. Firmware build and host tests are verified; target-board SD,
timing, and resource acceptance are **NEEDS HARDWARE ACCEPTANCE**.

## Ownership and integration

```text
APP_LOGx -> bounded structured formatting -> ESP-IDF console sink
                                       -> PSRAM byte ring -> log_writer -> SD VFS
main -> init/start and SD/time availability hint wiring
sd_card_manager -> mount/unmount/recovery and file leases
time_manager -> clock validity, timezone and SNTP
```

Only the writer opens, writes, syncs, closes, scans or deletes log files. It
acquires an SD lease before opening a file and releases it after closing. The
same lease protects retention directory handles. Recovery can reject new
leases immediately, then wait for the logger to close its file before unmount.
The logger reports classified media errors through the public SD API. Ordinary
filesystem failures, including ENOSPC, back off without forcing a remount.

Dependencies: public `log`; private `freertos`, `heap`, `esp_timer`,
`esp_hw_support`, `common`, `sd_card_manager`, `time_manager`. SD/time never
depend on the logger. `main` registers the single SD availability hint observer
and the existing single time status observer. Both callbacks only notify the
writer, outside provider locks. They borrow no transient status pointers.

Startup runs init/start at the beginning of `app_main`, before NVS/network,
display, SD or SNTP startup. PSRAM and the scheduler already exist there.
Initialization failure leaves console logging available and does not abort boot.
Only BOOT_START, DISPLAY_READY and sensor init/start logs were migrated. Existing
ESP-IDF, library, security, SD and other application logs remain unchanged.

## Producer interface

Include `app_log.h`; add `log_manager` to the consumer's CMake dependency list.

```c
static const char *TAG = "WIFI_MANAGER";
APP_LOGI(TAG, WIFI_CONNECTED, "rssi=%d", rssi);
APP_LOGD(TAG, SAMPLE, "sequence=%lu", (unsigned long)sequence);
```

`APP_LOGV/D/I/W/E` stringify the event token. `app_log_emit()` is the underlying
printf-checked function, also available when an event string is needed. Neither
an event enum nor component-local initialization is required.

```text
[UNSYNCED][+000143ms][I][MAIN_APP][BOOT_START][boot=0123456789abcdef] project=Smart_Room_Cloud_Gateway
[2026-09-07 13:21:45.382][+004812ms][D][SENSOR][SAMPLE][boot=0123456789abcdef] sequence=4
```

Uptime comes from `esp_timer_get_time()`. Local wall time uses system time only
after `time_manager_is_synced()`. Lines retain their capture-time timestamps
when buffered. Tags/events are bounded to 24/40 bytes; record size defaults to
512 bytes including newline and NUL. ASCII controls, including CR/LF and ESC,
become spaces. Oversize content ends in `truncated=1` and increments the counter.
Details should be key=value; context such as `gen=4` goes in details. Source file,
function and line metadata are not emitted.

Task context only: no ISR, panic or cache-disabled usage. An ISR call is rejected,
but argument evaluation still occurs before entering the function, so these
macros must not be treated as ISR-safe telemetry. Use ISR counters/notifications
and log later from a task. Producer stack cost includes the configured record
array plus libc formatting/time-conversion frames; measure callback/task stack
headroom before migrating more components.

## Buffer, scheduling and policy

The ring is a byte FIFO with two-byte length framing and complete-record eviction.
One PSRAM-required allocation contains the ring and a separate retry batch. There
is no internal-RAM fallback, per-record malloc/free, or runtime resizing. A
zero-wait mutex attempt protects producer ring access. Full ring drops oldest
queued records; mutex contention drops the incoming record and counts it.
An in-flight retry batch is protected from eviction. Thus V1 is DROP_OLDEST for
the ring, with at most one additional retained batch.

The writer wakes on threshold crossing, timeout, environment change, ERROR, or
management request. Task notifications coalesce repeated wakeups. It drains a
snapshot-sized budget so ongoing producers cannot indefinitely extend a flush.
Each batch groups records by captured date/time-validity to preserve transition
boundaries. No per-line SD writes or syncs are issued during ordinary draining.
Files use unbuffered stdio over already-batched writes; `fflush` plus `fsync`
implements the separate durability operation. Sync deadlines are checked during
large backlog drains as well as between wakes. SD latency can extend a deadline.

| Kconfig (`Component config -> Application log manager`) | Default |
|---|---:|
| Enable persistent logger (requires SPIRAM) | enabled |
| PSRAM ring | 512 KiB |
| Maximum record including newline/NUL | 512 bytes |
| Batch threshold / maximum batch | 4096 bytes |
| Write timeout | 1000 ms |
| Durability interval | 5000 ms |
| File rotation limit | 5120 KiB |
| Retained log data limit | 256 MiB |
| Console / storage levels | INFO / DEBUG |

Levels use IDF ordering: 0 disables; 1=ERROR through 5=VERBOSE. A record passes
when its level is at or below the configured maximum. Console and storage
enable/level settings are independent. Runtime storage disable prevents new
records, while previously accepted backlog continues draining. ERROR only sends
an asynchronous urgent hint; it never waits for SD sync in the producer.

Console uses the public `esp_log_write()` sink with a reserved `APP_LOG_SINK`
filter tag; the original application tag stays in the structured line. The
console setter updates only this reserved tag, never `*` or library tag levels.
The configured SDK supports dynamic per-tag levels, verified in its local
`log_write.c`, `log_level.c` and `tag_log_level.c`. Direct wrapper calls are not
compiled out by `CONFIG_LOG_MAXIMUM_LEVEL=INFO`; storage DEBUG and runtime console
VERBOSE work independently. In alternate SDK configurations without dynamic
per-tag control, IDF's console policy remains an additional restriction. Do not
externally override the reserved tag. No `esp_log_set_vprintf()` interception is
installed. Console retains normal ESP-IDF/UART blocking behavior, independent of
persistence's non-waiting ring access.

## Files, time transition and retention

Actual paths are under the existing `SD_MOUNT_POINT` (`/sdcard`):

```text
/sdcard/logs/unknown/boot_0123456789abcdef_000.log
/sdcard/logs/2026_09_07/12_50_33_boot_0123456789abcdef_001.log
/sdcard/logs/2026_09_07/12_50_38_boot_0123456789abcdef_002.log
```

The 64-bit random session ID is generated once per successful init and retained
across stop/start. It is probabilistic, not an NVS monotonic boot counter and not
a security token. Deinit/init creates a new session. `O_EXCL` and a bounded
collision retry prevent overwriting an existing file, even if an ID repeats.
The global session sequence advances on each file-open attempt, including
rotation/recovery. The time prefix is the first batch's captured time, so it may
change between segments. No old file is renamed or moved.

SD availability does not depend on time. Unknown records are written immediately
when storage is ready. On synchronization the writer queues a TIME_SYNC control
record (independent of the storage level, but respecting storage enable). When
crossing from an open unknown file to dated data, it also writes a TIME_SYNC
boundary marker to the old file, syncs/closes it, and opens the dated file.
This one boundary marker is an exception to ordinary batching and is not an
application-record counter entry. Space is reserved for it within the rotation
limit. Previously queued unknown records are drained before the transition.
An older capture enqueued late by a preempted producer retains UNSYNCED in its
line but cannot switch an already dated session back to an unknown file.
Without SNTP, unknown files continue rotating normally. Date changes also close
the current file and open the appropriate date directory.

Retention reserves one rotation-sized amount of headroom when starting each
file, keeping ordinary batch writes free from directory scans. It scans only
the `unknown`/date directories and this component's exact filename grammar,
then deletes the oldest CLOSED file by filesystem modification time. Ties use
lexical path order. The active file and unrelated files are never candidates.
FAT timestamps before synchronization are not reliable chronology; tie order is
deterministic rather than a claim about physical boot order. Repeated scans
use constant memory and stop on an error. V1 counts logical log file bytes,
not FAT allocation-unit overhead, unrelated SD data or empty directory metadata;
empty date directories are retained. A large archive may take noticeable writer
time to scan, while producers continue buffering/dropping. Retention assumes
this logger is the only writer under its log namespace. Rotation must fit inside
retention, otherwise init returns INVALID_ARG.

## Failure and durability semantics

After a write/sync/open/retention failure, close the handle, release the lease and
retain the unwritten batch. Media errors notify the SD owner; other filesystem
errors back off for five seconds. A readiness hint clears the retry delay. A
one-second provider-snapshot fallback covers a missed contended notification;
it is not high-frequency storage polling and does no media probe. Recovery opens
a fresh segment and retries the batch before newer queued data. Internal failure
diagnostics use console-only `ESP_LOGW`, never the persistent frontend.

This is best-effort logging, not a transaction journal. A partial media write may
leave a partial final line in the failed segment; replay of its retained batch in
a new segment may duplicate records. No further repair/truncate I/O is attempted
on failed storage. Successful `fwrite` increments persisted_records, but only a
successful sync asks the filesystem for durability; card controller behavior and
power loss remain hardware-dependent. A later sync failure cannot reconstruct
batches already accepted by the filesystem.

`flush`/`stop` return FAIL when storage is unavailable, even with an empty ring.
An unsynced background close or close failure is remembered across remount until
a management request reports the failed outcome. A later request can succeed for
current work; this does not restore data whose durability was previously uncertain.
Cleanup errors are propagated instead of being overwritten by a later empty close.

PSRAM backlog is NOT guaranteed to survive panic, watchdog reset, heap/stack
corruption or uncontrolled power loss. RTC/flash/coredump panic persistence is
outside V1.

## Management and lifecycle

All management is task-context code. One composition owner serializes init,
start, flush, stop and deinit. Never call a blocking management API from a
provider callback or the writer itself.

| API | Contract |
|---|---|
| `log_manager_init()` | Idempotent; allocates PSRAM, begins buffering even before start. Disabled build returns NOT_SUPPORTED. |
| `log_manager_start()` | Idempotent while running; creates one unpinned writer. Returns INVALID_STATE while stopping/uninitialized. |
| `log_manager_flush(timeout_ms)` | Bounded caller wait for snapshot drain/sync; OK, FAIL or TIMEOUT. Concurrent new logs need not be included. |
| `log_manager_stop(timeout_ms)` | Rejects new persistent records, drains, syncs/closes and exits. TIMEOUT leaves resources alive; repeat stop. FAIL reports unavailable storage, cleanup error or uncertain durability; unwritten backlog remains available for restart. |
| `log_manager_deinit()` | Requires stopped writer; frees PSRAM and counts remaining records as dropped. Idempotent. |
| `log_manager_get_stats(&stats)` | Copies under a bounded 20-ms mutex attempt; INVALID_ARG for NULL, TIMEOUT on contention/uninitialized lock. |
| Console/storage setters | Central enable/level policy; no SD I/O. |
| `log_manager_notify_environment_changed()` | Zero-wait task-context hint; no formatting or I/O. |

For a future controlled reboot owner, call `log_manager_stop(1000)` and handle its
result before the existing reboot decision. This change exposes that path but
does not alter the reset coordinator's policy or add an automatic shutdown hook.
A timeout bounds the caller, not the underlying FAT/SPI operation; forcibly
deleting a blocked writer or freeing its buffer is forbidden.

Counters cover storage-eligible record attempts, accepted records, ring eviction,
contention drops, retry recovery, size rotations and truncation. `buffered_bytes`
includes the ring payload and retry batch; `buffer_capacity` is the ring allocation
including framing. Counters reset on a new successful init, not on start. Explicit
TIME_SYNC records are counted; the old-file transition marker is not. Stop
suppresses new persistence; console remains independently usable. Static mutex
and completion semaphore storage deliberately remain for application lifetime,
so producers never race deletion of a lock during deinit.

## Resource budget and security

| Resource | Default allocation |
|---|---|
| PSRAM_REQUIRED ring + retry batch | 524288 + 4097 = **528385 bytes**, one allocation |
| INTERNAL_REQUIRED writer stack | **6144 bytes**, priority **2**, no affinity |
| INTERNAL_REQUIRED runtime | One TCB, two static semaphores, small state/path counters |
| Writer file handles | One active FILE; up to two DIR handles during retention |
| Producer stack | 512-byte record plus bounded time/printf frames |

Libc/VFS may allocate at file/directory lifecycle boundaries. There is no logger
allocation per record. FreeRTOS internal allocation behavior was verified against
the configured SDK's `freertos/heap_idf.c`; this component has no PSRAM task-stack
override. Actual heap deltas, stack high-water mark, CPU cost and SD contention
with audio/LVGL require measurement on the board.

Call sites must never pass passwords, PoP, tokens, authorization headers, private
keys or secret configuration. No fragile automatic redaction is attempted. The
small migrated set contains only public project identity and numeric sensor
configuration. Log files are plaintext on removable media; physical access is
not prevented by this component.

## Validation and hardware acceptance

```powershell
idf.py build
& components/system/log_manager/test/host/run_tests.ps1
idf.py -p <PORT> flash monitor
```

Host tests compile the actual buffer and writer source with `-Wall -Wextra
-Werror`, real temporary host files and pthread RTOS/device shims. Test data
stays under `build/host_log_manager_tests/<unique-run>/sd`; real SD content is
never touched. Tests use smaller capacities/intervals and inject device failures.
They also compile/run the disabled configuration. Host shims do not prove
FreeRTOS priority behavior, FAT/SPI timing or ESP32-S3 memory placement.

| Required check | Software evidence | Target hardware |
|---|---|---|
| 1. Build | Full ESP-IDF 6.0.1 firmware build PASS | Flash/boot pending |
| 2–3. Unsynced logging, then valid time | Unknown file, TIME_SYNC in both files, dated rollover PASS | Block SNTP, then permit sync |
| 4–5. SD absent, then available | Offline backlog and readiness wake PASS | Boot without card, insert card |
| 6–7. Write failure/recovery | Injected EIO, retry batch, fresh segment PASS | Remove/reinsert during logging |
| 8. Overflow | 10000 ring wrap/eviction iterations, bounded offline backlog PASS | Sustained SD outage |
| 9–11. Threshold, timeout, periodic sync | Independent timed assertions PASS | Measure batch/sync latency |
| 12–13. Rotation/retention | Small test limit, closed-file deletion, active/unowned protection, unlink error PASS | FAT archive and full-card test |
| 14–16. Sink policies | Console/storage toggles; DEBUG storage and VERBOSE console PASS | Observe UART and SD together |
| 17. Truncation | Marker, bound, newline sanitation and counter PASS | Inspect generated file |
| 18. Lifecycle | Idempotency, timeout/retry, restart, allocation/task failure PASS | Repeated lifecycle on FreeRTOS |
| 19. No recursion | Produced count stable across injected backend error PASS | Observe real media errors |
| 20. Resource cleanup | Host task/allocation/lease counts zero after deinit PASS | Heap, stack, VFS/TCB inspection |

Additional concurrency test runs four producers and checks final
`produced == persisted + dropped`, with an empty backlog. An induced mutex
collision verifies incoming-drop behavior. A slow injected write proves ERROR
does not await storage and stop TIMEOUT does not permit deinit.

Six durability regressions also cover flush/stop after write-before-sync with
offline storage, injected close EIO, and background close followed by remount.
They verify FAIL propagation, subsequent request recovery and resource cleanup.

On hardware, also keep audio playback/capture, LVGL SD reads and network activity
running while measuring internal/PSRAM heap, writer stack minimum, CPU and
audible glitches. Use a disposable test card/log directory for removal/full-card
tests. **No hardware acceptance is claimed.** The full build retains the existing
unrelated unused-function warning in `lvgl_image_handler.c`; it was not changed.

## Explicit assumptions / V1 choices

- The requested branch is the working branch; no branch, commit, push or PR is
  created. Both roadmaps remain unchanged.
- SD currently has no public observer; a single neutral availability callback
  was added and wired by `main`. The existing time callback slot was unused.
- A random session ID avoids taking NVS ownership; it is not a persistent counter.
- Unknown segments include sequence suffixes so rotation/recovery never overwrite.
- Date/time prefixes come from record capture; they need not remain identical
  across segments of one session. The boot ID is the correlation key.
- DROP_OLDEST applies to queued records; a protected retry batch and contention
  drop-new behavior are deliberate bounded-concurrency exceptions.
- Retention is size-based and may delete earlier than the exact limit because
  it reserves a full file of headroom. Empty directory cleanup and tag-specific
  application policies are not implemented.
- No broad log migration, panic subsystem, reboot-policy change or roadmap
  completion is part of this task.
