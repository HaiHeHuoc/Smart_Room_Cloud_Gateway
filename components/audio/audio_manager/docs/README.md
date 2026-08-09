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

`audio_manager_state_to_string()` provides stable text for logs and future GUI mapping.

## Public API

- `audio_manager_default_config()` returns the known-good Phase 11 defaults.
- `audio_manager_init()` validates/copies configuration, creates synchronization, allocates PSRAM buffers, and establishes the safe speaker state.
- `audio_manager_register_status_callback()` registers or removes one copied status callback for application integration.
- `audio_manager_start()` starts the single manager-owned record/DSP/playback stability task.
- `audio_manager_get_status()` copies the latest manager state and diagnostics under a bounded mutex wait.
- `audio_manager_deinit()` releases resources only before the infinite manager task has been started.
- `audio_manager_state_to_string()` converts one state enum to a stable string.

`audio_manager_test_start()` remains temporarily as a compatibility alias to `audio_manager_start()` so the current `app_main` integration does not need to change in the same structural refactor.

## GUI Integration Contract

The manager does not depend on `app_gui` or LVGL. A future application adapter should register one callback and only copy/post the status into the GUI queue.

```c
static void app_audio_status_callback(
    const audio_manager_status_t *status,
    void *user_context)
{
    (void)user_context;
    /* Map/copy status->state into the app_gui queue. */
}
```

The callback always executes in task context after the internal status mutex has been released. Lifecycle calls may invoke it in the caller task, while runtime state transitions invoke it in the audio manager task. It is never invoked from the I2S ISR callbacks. The callback must return quickly and must not call LVGL directly.

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

`audio_manager_status_t` exposes lifecycle/active state, latest cycle result, cycle counters, latest recorded sample count, RX overflow/read-timeout counts, TX queue-overflow/write-timeout/partial-write counts, maximum RX/TX blocking duration, and manager task stack high-water mark.

`audio_manager_get_status()` uses a bounded mutex wait. Internal manager state changes use the component-owned mutex and publish copied snapshots after releasing it.

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
- The compatibility `audio_manager_test_start()` alias should be removed after `app_main` migrates to `audio_manager_start()`.
- GUI mapping is prepared through the state/callback contract but is not implemented in this refactor.
