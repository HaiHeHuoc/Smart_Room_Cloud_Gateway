# audio_manager Component Notes

## Purpose

`audio_manager` owns the current Phase 11 audio foundation for the INMP441 microphone and MAX98357A speaker path. The implementation preserves the hardware-proven NewSolution behavior while exposing lifecycle state and a thread-safe status callback compatible with the rest of the Smart Room Cloud Gateway manager architecture. Phase 11.4.1 adds a private bounded WAV source reader without changing the proven I2S/DSP path.

## State Model

```text
UNINITIALIZED
    -> INITIALIZED
    -> IDLE
       -> RECORDING -> IDLE
       -> PROCESSING -> IDLE
       -> PLAYBACK -> IDLE
       -> ERROR -> cleanup/recovery -> IDLE
```

`audio_manager_state_to_string()` provides stable text for logs and the
application GUI mapping. `RECORDING` begins as soon as the manager enables RX,
so it includes microphone startup discard and slot detection as well as the
five-second retained recording.

## Public API

- `audio_manager_default_config()` returns the known-good Phase 11 defaults.
- `audio_manager_init()` validates/copies configuration, creates synchronization, allocates PSRAM buffers, and establishes the safe speaker state.
- `audio_manager_register_status_callback()` registers or removes one copied status callback for application integration.
- `audio_manager_start()` starts the single manager-owned record/DSP/playback stability task.
- `audio_manager_get_status()` copies the latest manager state and diagnostics under a bounded mutex wait.
- `audio_manager_deinit()` releases resources only before the infinite manager task has been started.
- `audio_manager_state_to_string()` converts one state enum to a stable string.

`audio_manager_test_start()` remains a compatibility alias for legacy callers.
The current `app_main` integration uses `audio_manager_start()`.

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

## Phase 11.4.1 SD/WAV Streaming Foundation

`audio_wav.c` and `audio_wav.h` are private implementation files, not public
`audio_manager` API. They establish the file-source side of future playback:

```text
mounted SD VFS path
    -> private RIFF/WAVE parser
    -> one bounded PCM16 reader buffer
    -> future manager-owned TX consumer
```

The manager now owns one private playback-source slot. The existing stability
loop explicitly selects its retained PCM24 recording as that source and releases
it during the normal cleanup path. The WAV stream slot remains unused until
Phase 11.4.2 adds source arbitration and connects bounded PCM to the existing
manager-owned TX path. No second audio task, I2S channel owner, GUI state, or
public API is introduced here.

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
mixing in this foundation.

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
reader is single-owner and not thread-safe; future calls must remain serialized
by the audio manager task/lifecycle owner.

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
- Phase 11.4.1 does not create a WAV playback task or call the existing TX
  helpers from the file reader.

## Status And Diagnostics

`audio_manager_status_t` exposes lifecycle state, explicit RX/TX I2S-active
flags, latest cycle result, cycle counters, the latest recorded sample count,
lifetime RX/TX requested and returned byte totals, RX overflow/read-timeout
counts, TX queue-overflow/write-timeout/partial-write counts, maximum RX/TX
blocking duration, and manager task stack high-water mark.

Task and ISR diagnostic updates are protected by a short component spinlock;
`audio_manager_get_status()` copies the resulting diagnostic snapshot while
holding its bounded status mutex. Internal manager state changes publish copied
snapshots only after releasing that mutex.

The current direct-DMA stability path has no manager-owned PCM queue. ESP-IDF
6.0.1 does not expose a safe public runtime fill level for its internal I2S
message queue, nor a hardware TX-underrun event. Therefore queue occupancy and
TX underrun are intentionally not reported as zero or inferred from queue
overflow/timeouts. They require the bounded playback ring planned for Phase
11.2.

## Current Stability Mode

The current Phase 11 NewSolution task intentionally repeats forever:

```text
record 5 s
  -> DSP
  -> play the same recording
  -> cleanup
  -> log diagnostics/resources
  -> repeat
```

This remains the golden hardware-soak path while the component architecture is normalized.

## Current Limitations

- Runtime stop/restart is not implemented.
- `audio_manager_deinit()` is valid only before `audio_manager_start()`.
- The current task is a stability flow, not the final source-agnostic streaming API required by later Xiaozhi work.
- The compatibility `audio_manager_test_start()` alias remains only for legacy callers and should be removed after those callers migrate.
- Live I2S queue occupancy and a true TX-underrun counter remain deferred until the Phase 11.2 manager-owned PCM ring exists.
- Phase 11.4.2 must arbitrate sources and feed the validated WAV reader into
  the manager-owned TX/ring path; end-to-end SD playback remains hardware
  validation work.
