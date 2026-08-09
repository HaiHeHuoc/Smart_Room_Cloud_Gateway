# audio_manager Component Notes

## Purpose

`audio_manager` owns the current Phase 11 audio foundation for the INMP441 microphone and MAX98357A speaker path. The implementation preserves the hardware-proven NewSolution behavior while exposing lifecycle state and a thread-safe status callback compatible with the rest of the Smart Room Cloud Gateway manager architecture. Phase 11.4.2 connects the private bounded WAV reader directly to the proven manager-owned TX path without changing the microphone DSP path.

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

## Phase 11.4.1/11.4.2 Bounded SD/WAV Playback

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
both source kinds. `audio_wav` never touches I2S. No producer task, consumer
task, PCM ring, extra queue, public playback API, GUI state, or second I2S owner
is introduced. A ring or task split is justified only if hardware measurements
show that bounded direct SD reads cannot keep the existing TX path fed.

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

### Phase 11.4.2 validation trigger

The temporary proof mechanism is off by default. In `idf.py menuconfig`, open
`Audio manager`, enable `Play one WAV at audio-task startup`, and set an
absolute path below `/sdcard`. On the next boot, the manager attempts that file
once, performs defensive cleanup on success or failure, then resumes the
unchanged infinite record/DSP/recorded-playback stability loop. This mechanism
does not replace the Phase 11.4.3 production control API.

One aggregate `WAV_DIAG` log reports accepted data bytes/duration, bytes read,
mono bytes submitted to TX, read count/failures, maximum `fread()` duration,
stream elapsed time, and TX requested/written/overflow/timeout/partial metrics.
No INFO log is emitted for every read.

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
- Phase 11.4.2 adds only a private WAV source branch; it does not modify the
  recorded-audio DSP, conditioning, TX configuration, or I2S helpers.

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
TX underrun are intentionally marked unavailable for this architecture rather
than reported as zero or inferred from queue-overflow/timeouts. Their absence
does not require or justify a PCM ring. If a future measured design introduces
an application-owned ring, its occupancy and starvation policy can add those
metrics then.

## Current Stability Mode

With the WAV validation option disabled (the default), the current Phase 11
NewSolution task intentionally repeats forever:

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
- Live I2S queue occupancy and a true hardware TX-underrun signal are not
  observable through the current ESP-IDF direct-DMA API.
- Phase 11.4.3 still needs a bounded production control/lifecycle API,
  source arbitration with stop/cancel semantics, and concurrency policy.
- End-to-end WAV sound quality, SD latency under Gateway load, and golden-path
  regression remain hardware validation work.
