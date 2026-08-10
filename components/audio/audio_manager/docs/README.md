# audio_manager Component Notes

## Purpose

`audio_manager` owns the Phase 11 audio foundation for the INMP441 microphone
and MAX98357A speaker path. The Phase 11.4.3 production task starts in `IDLE`,
accepts one copied WAV request at a time, and owns all WAV, source, and I2S
lifecycle work. The default production path no longer starts the infinite
record/DSP/playback soak. The hardware-proven NewSolution algorithms remain
available through a default-off golden stability mode.

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
five-second retained recording.

## Public API

- `audio_manager_default_config()` returns the known-good Phase 11 defaults.
- `audio_manager_init()` validates/copies configuration, creates synchronization, allocates PSRAM buffers, and establishes the safe speaker state.
- `audio_manager_register_status_callback()` registers or removes one copied status callback for application integration.
- `audio_manager_start()` starts the single manager-owned task and waits up to
  two seconds for it to reach production `IDLE`.
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

## Production Command And Ownership Model

```text
public audio_manager_play_wav(path)
    -> validate and copy at most 255 path bytes
    -> bounded internal PLAY_WAV command
    -> single audio_manager task
    -> select/open private WAV source
    -> bounded 4 KiB reads and proven TX writes
    -> manager-owned stop/close/free/speaker-LOW cleanup
    -> IDLE
```

The queue holds two fixed-size commands. Only one audio operation slot may be
reserved, so the second slot exists for cooperative `SHUTDOWN`, not a second
WAV or a playlist. `RECORDING`, `PROCESSING`, `PLAYBACK`, active golden
stability mode, and shutdown all reject a conflicting WAV request with
`ESP_ERR_INVALID_STATE`. The copied path is 256 bytes including its null
terminator, and callers may immediately reuse their path buffer after a
successful return.

Exactly one task owns I2S RX/TX, `s_tx_block`, the playback-source slot, the
`FILE *`, and the WAV reader buffer. Public callers only submit commands or set
a short critical-section-protected cancel/shutdown request. They never close a
file, delete an I2S channel, free a WAV buffer, or call LVGL.

Cancellation is polled before each WAV read, before each bounded PCM TX block,
and before each pre/post-silence TX block. The manager then exits through the
same first-error-preserving cleanup path. A normal user cancellation increments
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

## Phase 11.4.1-11.4.3 Bounded SD/WAV Playback

`audio_wav.c` and `audio_wav.h` are private implementation files, not public
`audio_manager` API. The Phase 11.4.2 path is:

```text
mounted SD VFS path
    -> private RIFF/WAVE parser
    -> one bounded PCM16 reader buffer
    -> explicit little-endian PCM16 decode
    -> mono duplicated to the existing stereo TX staging block
    -> existing write_tx_frames() / I2S TX
```

The manager owns one private playback-source slot for either retained PCM24 or
a WAV PCM16 stream. The same audio-manager task selects, plays, and releases
both source kinds. `audio_wav` never touches I2S. Phase 11.4.3 adds only the
small fixed command queue described above; it adds no producer/consumer task,
PCM ring, per-source queue, GUI state, or second I2S owner. A ring or task split
is justified only if target-hardware measurements show that bounded direct SD
reads cannot keep the existing TX path fed.

### SD ownership and paths

- `sd_card_manager` remains the sole owner of SDSPI, FATFS mount/unmount, and
  card hardware lifecycle.
- The reader checks `sd_card_manager_is_mounted()` then uses normal C stdio on
  an absolute VFS file path under `SD_MOUNT_POINT` (currently `/sdcard`).
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

Each open stream owns exactly one `FILE *` and one 4 KiB Internal-RAM buffer.
The buffer matches the current SDSPI 4 KiB transfer ceiling and holds 128 ms of
canonical PCM16 mono. It is allocated once at open, reused by every `fread()`,
and freed at close; it requires only Internal 8-bit RAM and is never passed to
I2S directly. It is not a full-WAV/PSRAM allocation. Playback memory is
therefore independent of WAV duration.

`audio_wav_stream_open()` closes the local file on every parse/allocation
failure. `audio_wav_stream_read()` treats a short read before the declared data
end as corruption or I/O failure and closes the stream before returning. Close
always releases the buffer even if `fclose()` reports an error. The private
reader is single-owner and not thread-safe; all calls remain serialized by the
audio manager task/lifecycle owner.

`fopen()` returning `ENOENT`/`ENOTDIR` maps to `ESP_ERR_NOT_FOUND`. An existing
RIFF file missing `fmt ` or `data` maps to `ESP_ERR_INVALID_RESPONSE` instead,
so a missing path and malformed content are distinguishable. Unsupported audio
formats map to `ESP_ERR_NOT_SUPPORTED`; bounds/truncation errors remain
deterministic invalid-size failures.

`audio_wav_parse_file()` remains separately callable inside the component so a
test harness can exercise valid PCM16 fixtures, metadata/odd-padding chunks,
truncated files, missing chunks, unsupported formats, and oversized chunk
lengths without requiring a mounted SD card.

| Fixture class | Expected parser result |
| --- | --- |
| PCM16 mono 16-kHz, including `JUNK`/`LIST` chunks | Accepted; data offset and payload length reported |
| Odd-sized unknown metadata chunk | Accepted after one-byte RIFF padding skip |
| Large canonical WAV | Accepted with the same 4 KiB stream buffer |
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
alignment/rate, and truncated payloads.

Validation evidence is deliberately separated:

| Check | Phase 11.4.3 result | What it proves |
| --- | --- | --- |
| Native parser suite | 23/23 passed | Private RIFF/WAV parser and bounded reader fixture behavior |
| ESP-IDF 6.0.1 firmware build | Passed | Firmware compiles and links with the production lifecycle APIs |
| Target WAV playback | Pending | Real SD latency, I2S TX, MAX98357A sound, EOF, cancellation, and cleanup |
| Golden MIC regression | Pending | Real INMP441 record, DSP, and recorded playback after the lifecycle refactor |

The host suite does not emulate I2S TX, MAX98357A, target SD latency, audible
continuity, or hardware cancellation. A firmware build also does not prove any
of those runtime behaviors.

### WAV output and TX lifecycle

WAV samples bypass microphone DC removal, HPF, LPF, adaptive noise suppression,
PCM24 conversion, 16x recorded-audio gain, fade, compression, and limiter. The
only WAV amplitude policy is the configured linear `playback_volume_percent`;
at 100 percent, each valid PCM16 sample is preserved. Each sample is decoded
without an unaligned cast and duplicated into the existing static
DMA-capable `s_tx_block`.

Both source kinds reuse the same TX lifecycle: create/configure I2S0, preload
DMA silence, enable TX, write the existing pre-playback silence, publish the
existing `PLAYBACK` state, stream through `write_tx_frames()`, write the
existing post-playback silence on normal completion, stop/delete TX, hold
MAX98357A DIN LOW, release the source, and return to `IDLE`. Normal WAV EOF is
success. The first stream/TX error is preserved while cleanup errors are still
reported.

### Default-off hardware regression hooks

Normal production startup reaches `IDLE` and waits for commands. Two mutually
exclusive options remain under `idf.py menuconfig` -> `Audio manager`:

- `Submit one WAV through production API at task startup` submits the configured
  `/sdcard/...` file once through `audio_manager_play_wav()`. It uses the same
  arbitration, command, cancellation, streaming, status, and cleanup path as an
  application request; there is no private validation playback implementation.
- `Run the golden record/DSP/playback stability loop` repeatedly calls the
  unchanged `run_cycle()` regression path. It owns the operation slot and
  observes cooperative manager shutdown between complete cycles.

Both options default off. Golden mode intentionally rejects production WAV
requests while active.

One aggregate `WAV_DIAG` log reports accepted data bytes/duration, bytes read,
mono bytes submitted to TX, successful read count/failures, maximum successful
`audio_wav_stream_read()` duration (`max_wav_read_us`),
stream elapsed time, and TX requested/written/overflow/timeout/partial metrics.
Failed wrapper calls do not inflate the maximum-read metric. It still measures
the successful bounded wrapper, including its mount-state check, rather than a
standalone raw `fread()` timer. No INFO log is emitted for every read.

Suggested hardware files are valid 5-second, 30-second, and 60-second PCM16
mono 16-kHz WAVs, followed by missing, bad-RIFF, stereo, 44.1-kHz, 24-bit, and
truncated cases. Verify sound continuity, clean EOF to `IDLE`, zero nominal TX
timeouts/partial writes/queue overflows, safe speaker inactivity afterward,
and then confirm the original five-second record/DSP/playback cycle still
works. These remain hardware checks until serial and listening evidence exists.

## Proven Audio Behavior Preserved

This structural refactor intentionally does not change the known-good audio path:

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
- Playback conditioning, fade, compression/limiting, and mono duplication remain unchanged.
- TX DMA silence preload and pre/post playback silence remain unchanged.
- MAX98357A data remains LOW while TX is inactive.
- RX/TX remain half-duplex with defensive cleanup every cycle.
- Phases 11.4.2/11.4.3 add the private WAV branch and production control around
  the shared TX lifecycle; they do not modify recorded-audio DSP, conditioning,
  I2S format/pins, DMA geometry, or golden capture policy.

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

The default Phase 11.4.3 task is command-idle:

```text
audio_manager_start()
  -> task ready
  -> IDLE
  -> wait for PLAY_WAV or SHUTDOWN
```

The default-off golden Kconfig mode still repeats:

```text
record 5 s
  -> DSP
  -> play the same recording
  -> cleanup
  -> log diagnostics/resources
  -> repeat
```

It calls the same preserved `run_cycle()` and remains the golden hardware-soak
path. It is a regression facility, not normal product behavior.

For the current target stress configuration,
`CONFIG_AUDIO_MANAGER_GOLDEN_STABILITY_MODE=y` selects a 60-second capture
(960000 samples / about 3.66 MiB PCM24 PSRAM) and task priority 6. Each cycle is
therefore: 60-second capture -> DSP -> recorded playback -> cleanup -> 250 ms
delay -> repeat. Priority 6 is intentionally one level above the priority-5 GUI
task; the existing bounded I2S calls and cooperative DSP yields remain in use.
Priority 7 is available only as a measured follow-up if the board retains GUI,
network, and watchdog responsiveness.

## Memory And Resource Budget

- Task stack: 8192 bytes, unchanged from the existing manager task.
- Command queue: two fixed commands; each contains one enum plus a 256-byte
  copied path (520 bytes of payload total with the current 4-byte enum, plus
  FreeRTOS queue metadata/storage alignment).
- Active WAV stream: one reusable 4096-byte Internal/8-bit buffer and one
  `FILE *`; both are released after EOF, cancel, or error.
- PSRAM: the whole-recording PCM24 buffer and DSP workspace remain allocated at
  init exactly as required by the preserved golden path. For the default
  five-second configuration, PCM24 history is 320000 bytes.
- DMA/Internal staging: existing static RX/TX/silence blocks and I2S DMA
  geometry are unchanged. No whole-WAV allocation, per-chunk allocation, PCM
  ring, or second task was added.

## Current Limitations

- Live I2S queue occupancy and a true hardware TX-underrun signal are not
  observable through the current ESP-IDF direct-DMA API.
- WAV support remains PCM16 mono at 16 kHz; there is no resampling, compressed
  codec, stereo-file support, playlist backlog, or network source.
- Cancellation cannot preempt a synchronous SD/VFS read or I2S write. A wedged
  media read has no component-level deadline; manager stop reports its finite
  five-second timeout without force-deleting the task.
- Golden stability shutdown is checked between complete `run_cycle()` calls,
  so the configured stress capture/playback cycle (60 seconds in the current
  local configuration) can outlast the public stop wait and require a later
  stop-status check.
- End-to-end WAV sound quality, SD latency under Gateway load, and golden-path
  MIC regression remain target-hardware validation work for Phase 11.4.4.
