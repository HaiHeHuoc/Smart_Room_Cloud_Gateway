# audio_manager Component Notes

## Purpose

`audio_manager` owns the Phase 11 audio foundation for the INMP441 microphone
and MAX98357A speaker path. The Phase 11 production task starts in `IDLE`,
accepts one copied WAV request at a time, and owns all WAV, source, and I2S
lifecycle work. The default production path no longer starts the infinite
record/DSP/playback soak. The hardware-proven NewSolution algorithms remain
available through a default-off golden stability mode. The optional continuous
WAV stress hook is disabled by default with
`CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST`; when enabled, its second, test-only
coordinator task only polls status and submits commands, and does not own I2S,
a file, or an SD lease.

## State Model

```text
UNINITIALIZED
    -> audio_manager_init() -> INITIALIZED
    -> audio_manager_start() -> IDLE
       -> RECORDING -> IDLE
       -> PROCESSING -> IDLE
       -> PLAYBACK -> IDLE
       -> ERROR -> cleanup/recovery -> IDLE
IDLE -> audio_manager_stop() -> INITIALIZED
INITIALIZED -> audio_manager_deinit() -> UNINITIALIZED
```

`audio_manager_state_to_string()` provides stable text for logs and the
application GUI mapping. `RECORDING` begins as soon as the manager enables RX,
so it includes microphone startup discard and slot detection as well as the
configured retained recording (five seconds by default; 60 seconds when the
optional golden stress profile is selected).

## Public API

- `audio_manager_default_config()` returns the known-good Phase 11 defaults.
- `audio_manager_init()` validates/copies configuration, creates synchronization, allocates PSRAM buffers, and establishes the safe speaker state.
- `audio_manager_register_status_callback()` registers or removes one copied status callback for application integration.
- `audio_manager_start()` starts the single I2S-owning manager task and waits
  up to two seconds for it to reach production `IDLE`. It never starts a
  continuous test coordinator by itself.
- `audio_manager_play_wav()` validates and copies one `/sdcard/...` path into
  bounded command storage; it never opens the file or waits for playback.
- `audio_manager_stop_playback()` requests cancellation of the pending/active
  WAV operation without touching its file, buffer, TX channel, or source.
- `audio_manager_stop()` requests cooperative shutdown and waits up to five
  seconds; success returns the lifecycle to `INITIALIZED`.
- `audio_manager_get_status()` copies the latest manager state and diagnostics under a bounded mutex wait.
- `audio_manager_deinit()` releases PSRAM, queue, event group, mutex, and audio
  resources after the task has stopped.
- `audio_manager_state_to_string()` converts one state enum to a stable string.

The obsolete `audio_manager_test_start()` alias had no branch callers and was
removed because production `audio_manager_start()` no longer means stability
soak. Public lifecycle/control APIs are task-context APIs, not ISR APIs.
Application code should serialize concurrent `start`, `stop`, and `deinit`
calls. Busy audio requests are rejected rather than accumulated as a playlist.

## Public-API Hardware Stress Coordinator

Enable `CONFIG_AUDIO_MANAGER_PUBLIC_API_TEST` only for a target-hardware
validation run. It causes `main` to call `app_audio_api_test_task_start()`,
which starts the test-only, priority-6 coordinator implemented in
`audio_api_test_task.c`. It calls only public `audio_manager` APIs and polls
copied status; it does not own I2S, PCM buffers, WAV files, or SD leases.

With the default `n` setting, no continuous record/playback/WAV coordinator is
created during normal production startup.

With the default local test selection, each cycle exercises fixed recording and
manual recording followed by recorded playback, then attempts the configured
WAV. If `sd_card_manager_is_mounted()` is false at the WAV step, the
coordinator logs `WAV skipped`, reports the cycle as `PARTIAL`, and starts the
next record/playback cycle after its configured delay. It neither mounts the
card nor waits indefinitely for SD readiness. Once SD recovery returns VFS to
READY, a later cycle automatically attempts WAV playback again.

## Production Command And Ownership Model

```text
public audio_manager_play_wav(path)
    -> validate and copy at most 255 path bytes
    -> bounded internal PLAY_WAV command
    -> single audio_manager task
    -> start private WAV reader/prefetch worker
    -> reader fills two bounded PSRAM PCM blocks from raw reads of at most 4 KiB
    -> transient media error: close/release -> SD remount -> fresh reopen/seek
    -> manager maps PCM16 full scale to the shared +/-9000 output ceiling
    -> manager consumes READY blocks through proven TX writes
    -> manager-owned stop/close/free/speaker-LOW cleanup
    -> IDLE
```

The queue holds two fixed-size commands. Only one audio operation slot may be
reserved, so the second slot exists for cooperative `SHUTDOWN`, not a second
WAV or a playlist. `RECORDING`, `PROCESSING`, `PLAYBACK`, normal golden
stability mode, and shutdown all reject a conflicting WAV request with
`ESP_ERR_INVALID_STATE`. When continuous WAV stress is combined with golden
mode, the scheduler releases the slot only after a complete golden cleanup and
handles its coordinator's accepted WAV before the next golden cycle. The copied
path is 256 bytes including its null terminator, and callers may immediately
reuse their path buffer after a successful return.

The audio-manager task is the sole owner of I2S RX/TX, `s_tx_block`, fixed WAV
PCM16 amplitude mapping, and the playback-source lifecycle. Its private
`wav_prefetch` worker is the sole owner of the WAV `FILE *`, raw 4 KiB reader
buffer, and SD VFS lease. The manager only consumes immutable READY PCM bytes
from PSRAM and joins the worker before destroying those buffers. Public callers
only submit commands or set a short critical-section-protected cancel/shutdown
request. They never close a file, delete an I2S channel, free a WAV buffer, or
call LVGL. The optional continuous-WAV coordinator is equivalent to a public
caller: it has no direct I2S/VFS access and stops through the same component
lifecycle.

Cancellation is polled before each 4 KiB worker read, before each bounded PCM
TX block, and before each pre/post-silence TX block. The manager then exits
through the same first-error-preserving cleanup path. A normal user cancellation increments
`wav_playback_cancelled`, keeps `last_error == ESP_OK`, and returns to `IDLE`
without publishing a false `ERROR`.

Cancellation is cooperative, not preemptive. It is observed after the current
synchronous SD/VFS read or I2S write returns. I2S writes use the existing
1000 ms timeout; the C stdio/SD layer has no component-enforced per-read
deadline, so an absolute cancellation bound cannot be promised for wedged
media. Manager shutdown waits five seconds, returns `ESP_ERR_TIMEOUT` if the
task is still cleaning up, leaves shutdown requested, and never force-deletes
the resource-owning task. A later `audio_manager_stop()` may observe completion.

## GUI Integration Contract

The manager does not depend on `app_gui` or LVGL. The application composition
registers one callback in `main`, maps the copied `audio_manager_status_t` to
`ui_audio_status_t`, and posts it to the length-one GUI queue.

```c
static void app_audio_status_callback(
    const audio_manager_status_t *status,
    void *user_context)
{
    (void)user_context;
    /* Map/copy status->state into the app_gui queue. */
}
```

The callback always executes in task context after the internal status mutex
has been released. Lifecycle calls may invoke it in the caller task, while
runtime state transitions invoke it in the audio manager task. It is never
invoked from the I2S ISR callbacks. The callback must return quickly and must
not call LVGL directly. Unregistration prevents future callback selection but
does not wait for an already copied/in-flight callback; callers must keep its
context valid until they have externally synchronized with that callback.

Suggested GUI meanings:

| Audio state | GUI meaning |
| --- | --- |
| `UNINITIALIZED` | Audio unavailable |
| `INITIALIZED` | Audio ready but task not started |
| `IDLE` | Audio ready/idle |
| `RECORDING` | Listening/recording |
| `PROCESSING` | Processing speech/audio |
| `PLAYBACK` | Speaker/output active |
| `ERROR` | Audio fault |

The current dashboard renders these concise labels: `Audio: --`, `Audio: Ready`,
`Audio: Idle`, `Audio: REC`, `Audio: DSP`, `Audio: PLAY`, and `Audio: ERR`.

## Phase 11.4 Bounded SD/WAV Playback

`audio_wav.c` and `audio_wav.h` are private implementation files, not public
`audio_manager` API. The Phase 11.4 WAV playback path is:

```text
mounted SD VFS path
    -> private RIFF/WAVE parser
    -> private reader fills a pair of bounded PCM16 PSRAM blocks
    -> single I2S owner consumes only READY blocks
    -> explicit little-endian PCM16 decode
    -> fixed full-scale PCM16 map to the shared +/-9000 ceiling
    -> mono duplicated to the existing stereo TX staging block
    -> existing write_tx_frames() / I2S TX
```

The manager owns one private playback-source slot for either retained PCM24 or
a WAV PCM16 stream. Recorded PCM remains manager-task-only. For WAV, the
private reader owns the stream/VFS lease and moves bytes through a two-slot
`FREE -> FILLING -> READY -> CONSUMING -> FREE` handoff; the manager remains
the only I2S owner. `audio_wav` never touches I2S, and the reader never touches
I2S or LVGL. The split is bounded to two logical blocks and does not introduce
a whole-file allocation, a public queue, GUI coupling, or a second I2S owner.

### SD ownership and paths

- `sd_card_manager` remains the sole owner of SDSPI, FATFS mount/unmount, and
  card hardware lifecycle.
- The reader atomically acquires an SD VFS lease before `fopen()` and releases
  it only after `fclose()`, before it signals its terminal event. The manager
  joins that event before freeing PSRAM or accepting the next source, which
  prevents SD recovery from unmounting a VFS beneath a live WAV stream.
- During recovery, new WAV opens return `ESP_ERR_INVALID_STATE`; the regular
  record/DSP/playback stress cycle remains SD-independent.
- A confirmed streaming media error is never retried on the failed `FILE *`.
  The reader closes it and releases its lease before waiting up to five seconds
  for `sd_card_manager` to remount. It may then open a fresh handle once, verify
  that all WAV metadata is unchanged, seek to the last committed PCM byte, and
  continue filling the same PSRAM slot. This order prevents a recovery deadlock
  and avoids FatFS's sticky per-file error state.
- The reader uses normal C stdio on an absolute VFS file path under
  `SD_MOUNT_POINT` (currently `/sdcard`).
- `S:/...` paths and `lvgl_sd_fs` are LVGL-only and are never used here.
- The helper does not call `sd_card_manager_init()`, FATFS mount APIs, SPI APIs,
  or LVGL.

### Accepted WAV contract

Only this format is accepted:

| Field | Required value |
| --- | --- |
| Container | `RIFF` / `WAVE`, little-endian |
| Encoding | PCM integer (`audio_format == 1`) |
| Channels | 1 (mono) |
| Sample rate | 16,000 Hz |
| Sample width | 16 bits |
| Block alignment / byte rate | 2 bytes / 32,000 bytes per second |

Stereo, float, ADPCM/compressed, 8/24/32-bit, and non-16-kHz inputs return a
deterministic unsupported-format error. There is no resampling or channel
mixing beyond duplicating the accepted mono sample into left and right TX
slots.

### Parser, memory, and error contract

The parser reads the RIFF header then iterates every chunk. It locates `fmt `
and `data` without assuming a 44-byte header, safely skips `LIST`, `JUNK`, and
unknown chunks, and applies required even-byte chunk padding. All chunk end
offsets are checked against both the RIFF-declared and actual file bounds.

Each open stream owns exactly one `FILE *` and one reusable 4 KiB Internal-RAM
raw reader buffer. The worker reuses it for each bounded `fread()` and copies
the result into the inactive logical PSRAM block. At the default
`AUDIO_MANAGER_WAV_PREFETCH_SECONDS=10`, PCM16 mono 16 kHz needs 32,000 B/s,
so each block is 320,000 B and the two-slot cache is 640,000 B. These buffers
are allocated only for active WAV playback, checked as external PSRAM, and are
never passed directly to I2S; the manager converts/copies into the existing
DMA-capable `s_tx_block`. Playback memory remains bounded and independent of
the total WAV-file duration.

`audio_wav_stream_open()` closes the local file and releases its lease on every
parse/allocation failure. `audio_wav_stream_read_limited()` lets the worker end
a logical cache block exactly at its configured boundary, while retaining the
4 KiB raw-read cap. A short read before the declared data end is corruption or
I/O failure and closes the stream before returning. A confirmed VFS I/O error
is reported to `sd_card_manager` before the lease is released. Close always
releases the raw buffer even if `fclose()` reports an error, and releases the
lease only after the FILE is closed. The private reader is single-owner and
not thread-safe; the manager never calls its open/read/close APIs concurrently.

The reader commits bytes only after a complete raw read. If a failed `fread()`
returned partial bytes, those bytes are discarded; a successful fresh-file
resume rereads from the last committed aligned offset, so PCM is neither lost
nor duplicated. A second error, a five-second recovery timeout, changed WAV
metadata, truncation, or an invalid resume offset ends that playback cleanly.

`fopen()` returning `ENOENT`/`ENOTDIR` maps to `ESP_ERR_NOT_FOUND`. An existing
RIFF file missing `fmt ` or `data` maps to `ESP_ERR_INVALID_RESPONSE` instead,
so a missing path and malformed content are distinguishable. Unsupported audio
formats map to `ESP_ERR_NOT_SUPPORTED`; bounds/truncation errors remain
deterministic invalid-size failures.

An `fopen()` errno classified by `sd_card_manager_is_vfs_media_error()` is
reported to recovery; expected missing-path errors are not.

`audio_wav_parse_file()` remains separately callable inside the component so a
test harness can exercise valid PCM16 fixtures, metadata/odd-padding chunks,
truncated files, missing chunks, unsupported formats, and oversized chunk
lengths without requiring a mounted SD card.

| Fixture class | Expected parser result |
| --- | --- |
| PCM16 mono 16-kHz, including `JUNK`/`LIST` chunks | Accepted; data offset and payload length reported |
| Odd-sized unknown metadata chunk | Accepted after one-byte RIFF padding skip |
| Large canonical WAV | Accepted with the same 4 KiB raw reader and bounded two-slot PSRAM cache |
| Random file, truncated RIFF, missing `fmt ` or `data` | Deterministic malformed/missing error |
| Stereo, 44.1-kHz, 8/24/32-bit, float, ADPCM | Deterministic unsupported-format error |
| Declared data/chunk length beyond file bounds | Deterministic invalid-size error |

The native host harness calls the real `audio_wav_parse_file()` implementation
with temporary normal files, so it requires neither an SD card nor I2S:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File components\audio\audio_manager\test\host\run_tests.ps1
```

It covers canonical, `JUNK`, `LIST`, unknown, odd-padded, data-before-format,
and large WAV fixtures plus random/truncated containers, missing chunks, zero
data, oversized chunks, unsupported channels/rates/widths/float, invalid
alignment/rate, truncated payloads, and aligned/EOF/invalid resume offsets.

Validation evidence is deliberately separated:

| Check | Current result | What it proves |
| --- | --- | --- |
| Native WAV suite | Parser 32/32 plus stream-contract/fixed-scale test passed | Parser, open/error SD-lease fixture behavior, resume-seek bounds, and fixed full-scale PCM16 endpoint/sample mapping; it does not execute the FreeRTOS prefetch worker |
| ESP-IDF 6.0.1 firmware build | Passed | Firmware compiles and links the fixed full-scale WAV mapping with the production lifecycle APIs |
| Target WAV playback | Pending | Real SD latency, PSRAM cache handoff, I2S TX, MAX98357A sound, EOF, cancellation, and cleanup |
| Golden MIC regression | Pending | Real INMP441 record, DSP, and recorded playback after the lifecycle refactor |

The host suite does not emulate FreeRTOS queues/tasks, PSRAM cache handoff,
I2S TX, MAX98357A, target SD latency, audible continuity, or hardware
cancellation. A firmware build also does not prove any of those runtime
behaviors.

### WAV output and TX lifecycle

WAV samples bypass microphone DC removal, HPF, LPF, adaptive noise suppression,
PCM24 conversion, 16x recorded-audio gain, fade, compression, and limiter. The
manager instead applies one stateless fixed full-scale PCM16 map before the
configured linear `playback_volume_percent`:

```text
output = round(input_pcm16 * 9000 / 32768)
```

The shared `AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16` define supplies 9000, and the
equivalent Q16 gain is 18000. `-32768` maps to `-9000`; `+32767` maps to no
more than `+9000`. The final volume path also clamps to the same ceiling. This
does not scan, seek, cache, create, or modify a WAV file, and it introduces no
per-block/dynamic limiter gain change. It preserves each source's waveform
shape, but does not normalize loudness: quieter files remain quieter. Each
sample is decoded without an unaligned cast and duplicated into the existing
static DMA-capable `s_tx_block`.

Both source kinds reuse the same TX lifecycle: create/configure I2S0, preload
DMA silence, enable TX, write the existing pre-playback silence, publish the
existing `PLAYBACK` state, stream through `write_tx_frames()`, write the
existing post-playback silence on normal completion, stop/delete TX, hold
MAX98357A DIN LOW, release the source, and return to `IDLE`. Normal WAV EOF is
success. The first stream/TX error is preserved while cleanup errors are still
reported. Reader outcome and lifecycle cleanup are separate: a successful
join/ACK/free returns cleanup success even when the earlier read operation
failed, so `WAV cleanup also failed` now denotes a real teardown failure only.

### Default-off hardware regression hooks

Normal production startup reaches `IDLE` and waits for commands. The following
test-only options remain under `idf.py menuconfig` -> `Audio manager`:

- `Submit one WAV through production API after SD is ready` is mutually
  exclusive with golden mode and continuous WAV stress. It waits for the
  configured `/sdcard/...` VFS to become ready, then submits that file once
  through `audio_manager_play_wav()`. It uses the same arbitration, command,
  cancellation, streaming, status, and cleanup path as an application request;
  there is no private validation playback implementation.
- `Run the golden record/DSP/playback stability loop` repeatedly calls the
  unchanged `run_cycle()` regression path. Without continuous WAV stress, it
  owns the operation slot and rejects production WAV requests.
- `Run a continuous WAV playback stress coordinator` has no I2S or VFS
  ownership. With golden mode disabled, it waits for SD readiness, repeatedly
  plays its configured WAV to EOF/failure/cancellation, then sleeps for the
  configured 60 seconds before its next submission. When combined with golden
  mode, it uses the post-cycle command window instead.

All hooks default off. Standalone continuous WAV stress does not capture or
run DSP. When combined with golden mode, it serializes full WAV playback
between golden cycles; it never records and plays a WAV simultaneously.

One aggregate `WAV_DIAG` log reports accepted data bytes/duration, fixed Q16
gain, observed post-volume output peak, raw bytes read, mono bytes submitted to
TX, raw read (at most 4 KiB) count/failures and maximum read duration, logical
prefetch block size/fills/failures/maximum-fill time, fresh-file SD resume
offset/attempt/success/wait, initial preload latency, total boundary wait,
software prefetch-starvation count, reader stack high-water mark, stream elapsed
time, and TX requested/written/queue-overflow/timeout/partial metrics.
`prefetch_starve` means the consumer had no READY block after a 100 ms boundary
poll. The manager then fails that WAV operation and stops TX rather than waiting
indefinitely with an empty cache. It is not a hardware I2S-underrun signal. No
INFO log is emitted for every raw read.

Suggested hardware files are valid 5-second, 30-second, and 60-second PCM16
mono 16-kHz WAVs, followed by missing, bad-RIFF, stereo, 44.1-kHz, 24-bit, and
truncated cases. At volume 100, confirm `fixed_gain_q16=18000` and
`output_peak <= 9000`; for a source with peak 27752, the expected output peak
is about 7622. For a 30/60-second file, listen carefully across every 10-second
boundary and confirm `expected_bytes == read_bytes == streamed_bytes`,
`prefetch_fill_fail=0`, and `prefetch_starve=0`. Check that each successful
`max_prefetch_fill_us` remains comfortably below the available 10-second cache
margin. Also cancel during initial fill and active playback, then induce an SD
read failure during refill and verify that the same WAV resumes from its
committed offset through a fresh file handle. For one transient failure, expect
`sd_resume_attempt=1`, `sd_resume_ok=1`, exact expected/read/streamed byte
parity, and no false cleanup
error; `max_prefetch_fill_us` includes the recovery wait. Without an injected
fault, both resume counters remain zero. Separately force a recovery timeout or
second read failure and verify clean terminal failure before the next stress
iteration. Verify clean EOF to `IDLE`, zero nominal TX timeouts/partial writes,
safe speaker inactivity afterward, and then confirm the configured golden
record/DSP/playback cycle still works. These remain hardware checks until
serial and listening evidence exists.

## Proven Audio Behavior Preserved

This implementation preserves the known-good microphone DSP and audio transport.
The intentional amplitude change is the shared
`AUDIO_DSP_OUTPUT_PEAK_CEILING_PCM16` value of 9000 for recorded playback and
fixed full-scale WAV mapping:

- I2S0 master ownership remains private to `audio_manager`.
- RX remains Philips standard I2S, 32-bit stereo, 16 kHz.
- TX remains Philips standard I2S, signed PCM16 stereo, 16 kHz.
- DMA remains 8 descriptors with 256 frames per descriptor.
- Startup discard remains 40 blocks.
- Left/Right slot detection remains 20 blocks.
- Selected microphone data remains packed PCM24-in-int32.
- Whole-recording PCM24 history and DSP workspace remain in PSRAM.
- RX/TX staging remains static DMA-capable internal memory.
- DSP remains DC removal, HPF80 x2, LPF6k x2, adaptive spectral NS, and cooperative task yielding.
- Recorded playback conditioning, fade, compression/limiting, and mono
  duplication remain unchanged apart from its hard ceiling now using the shared
  9000 define.
- TX DMA silence preload and pre/post playback silence remain unchanged.
- MAX98357A data remains LOW while TX is inactive.
- RX/TX remain half-duplex with defensive cleanup every cycle.
- The bounded WAV branch adds a fixed PCM16 amplitude map after SD-to-PCM
  handoff. It does not modify recorded-audio DSP algorithms, I2S format/pins,
  DMA geometry, or golden capture policy.

## Status And Diagnostics

`audio_manager_status_t` exposes lifecycle state, explicit RX/TX I2S-active
flags, latest manager-operation result, golden-cycle counters, four WAV
started/completed/failed/cancelled counters, the latest recorded sample count,
lifetime RX/TX requested and returned byte totals, RX overflow/read-timeout
counts, TX queue-overflow/write-timeout/partial-write counts, maximum RX/TX
blocking duration, and manager task stack high-water mark. The existing
`cycles_*` and `last_samples_recorded` fields remain golden-stability-only.

Task and ISR diagnostic updates are protected by a short component spinlock;
`audio_manager_get_status()` copies the resulting diagnostic snapshot while
holding its bounded status mutex. Internal manager state changes publish copied
snapshots only after releasing that mutex.

The current direct-DMA stability path has no manager-owned PCM queue. ESP-IDF
6.0.1 does not expose a safe public runtime fill level for its internal I2S
message queue, nor a hardware TX-underrun event. Therefore queue occupancy and
TX underrun are intentionally marked unavailable for this architecture rather
than reported as zero or inferred from queue-overflow/timeouts. Their absence
does not require or justify a PCM ring. If a future measured design introduces
an application-owned ring, its occupancy and starvation policy can add those
metrics then.

## Production And Golden Modes

The default Phase 11.4 task is command-idle:

```text
audio_manager_start()
  -> task ready
  -> IDLE
  -> wait for PLAY_WAV or SHUTDOWN
```

The default-off golden Kconfig mode still repeats:

```text
record configured duration
  -> DSP
  -> play the same recording
  -> cleanup
  -> log diagnostics/resources
  -> repeat
```

It calls the same preserved `run_cycle()` and remains the golden hardware-soak
path. It is a regression facility, not normal product behavior.

For the current local WAV-only stress configuration,
`CONFIG_AUDIO_MANAGER_GOLDEN_STABILITY_MODE` is disabled, so no 60-second
capture, DSP, or recorded playback runs. `CONFIG_AUDIO_MANAGER_WAV_STRESS_TESTAPP=y`
starts a priority-6 manager task and a priority-6 test coordinator for
`/sdcard/audio/input.wav`. Its schedule is:

```text
WAV-only: full WAV playback -> EOF/error/cancellation -> cleanup
      -> sleep 60 s
      -> submit and play the full WAV again
```

The coordinator does not own I2S, and the manager plays the WAV exclusively
until EOF/error/cancellation. A long file therefore plays in full before the
60-second post-completion delay starts. Priority 6 is intentionally one level
above the priority-5 GUI task; the existing bounded I2S calls remain in use.
Priority 7 is available only as a measured follow-up if the board retains GUI,
network, and watchdog responsiveness.

## Memory And Resource Budget

- Task stack: 8192 bytes, unchanged from the existing manager task.
- Continuous WAV stress only: one 3072-byte coordinator stack. It holds no WAV data,
  I2S channel, source slot, or SD lease and is woken immediately by
  `audio_manager_stop()` rather than waiting out its 60-second sleep.
- Command queue: two fixed commands; each contains one enum plus a 256-byte
  copied path (520 bytes of payload total with the current 4-byte enum, plus
  FreeRTOS queue metadata/storage alignment).
- Active WAV stream: one reusable 4096-byte Internal/8-bit raw reader buffer,
  one `FILE *`, and two configured-size PSRAM cache blocks. At the default
  ten-second setting the blocks total 640000 bytes. All are released after the
  reader has stopped at EOF, cancellation, or error.
- WAV prefetch worker: one private 4096-byte stack at priority 5. It owns only
  SD/VFS and cache fill; it never owns I2S or LVGL.
- PSRAM: the whole-recording PCM24 buffer and DSP workspace remain allocated at
  init exactly as required by the preserved golden path. For the default
  five-second configuration, PCM24 history is 320000 bytes.
- DMA/Internal staging: existing static RX/TX/silence blocks and I2S DMA
  geometry are unchanged. There is no whole-WAV allocation or second
  I2S-owning task; cache blocks are allocated once per WAV and reused until
  that source ends.

## Current Limitations

- Live I2S queue occupancy and a true hardware TX-underrun signal are not
  observable through the current ESP-IDF direct-DMA API.
- WAV support remains PCM16 mono at 16 kHz; there is no resampling, compressed
  codec, stereo-file support, playlist backlog, or network source.
- Fixed full-scale WAV mapping guarantees the shared output ceiling without a
  source scan, but it is a safety scale rather than loudness normalization:
  quieter WAV files remain quieter at the same configured volume.
- Cancellation cannot preempt one synchronous raw SD/VFS read or I2S write.
  The worker checks cancellation between raw reads, but a wedged media read has
  no component-level deadline; manager stop reports its finite five-second
  timeout without force-deleting the task or freeing a live reader buffer.
- Automatic in-operation SD recovery is intentionally bounded to one fresh-file
  resume and a five-second READY wait. Persistent media errors, a damaged file
  or sector, or a slower remount fail the current playback; continuous stress
  may submit a new iteration after its configured delay.
- Optional golden stability shutdown is checked between complete `run_cycle()`
  calls, so a configured long capture/playback cycle can outlast the public
  stop wait and require a later stop-status check. The optional WAV coordinator
  leaves its retry/sleep wait immediately when `audio_manager_stop()` signals
  it, but it cannot preempt an active worker-owned WAV read or manager-owned
  TX write.
- End-to-end WAV sound quality, SD latency under Gateway load, and golden-path
  MIC regression remain target-hardware validation work for Phase 11.4.4.
