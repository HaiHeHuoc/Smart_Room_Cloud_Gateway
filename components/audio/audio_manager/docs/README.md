# audio_manager Component Notes

## Purpose

`audio_manager` owns the current Phase 11 audio foundation for the INMP441 microphone and MAX98357A speaker path. The implementation preserves the hardware-proven NewSolution behavior while exposing lifecycle state and a thread-safe status callback compatible with the rest of the Smart Room Cloud Gateway manager architecture.

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
