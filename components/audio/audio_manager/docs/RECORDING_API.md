# Audio Manager Recording API

## Goal

The production `audio_manager` exposes retained microphone recording without
changing the proven INMP441 -> DSP -> MAX98357A algorithms. The manager task
remains the sole I2S RX/TX owner; callers only submit asynchronous operations or
set bounded cooperative stop requests.

## Public operations

```c
/* Fixed duration from audio_manager_config_t.record_duration_seconds. */
esp_err_t audio_manager_record(void);

/* Manual / push-to-talk capture. */
esp_err_t audio_manager_start_recording(void);
esp_err_t audio_manager_stop_recording(void);

/* Replay the latest successfully processed retained recording. */
esp_err_t audio_manager_play_recorded(void);

/* Existing WAV playback API. */
esp_err_t audio_manager_play_wav(const char *path);
esp_err_t audio_manager_stop_playback(void);
```

All control APIs are task-context APIs. Do not call them directly from an ISR.
A GPIO/button ISR should notify an application task first; that task may then
call `audio_manager_start_recording()` or `audio_manager_stop_recording()`.

## Fixed-duration recording

`audio_manager_record()` queues one production record command and returns after
acceptance. The capture duration is the configuration copied by
`audio_manager_init()`:

```c
audio_manager_config_t config = audio_manager_default_config();
config.record_duration_seconds = 5U;

ESP_ERROR_CHECK(audio_manager_init(&config));
ESP_ERROR_CHECK(audio_manager_start());
ESP_ERROR_CHECK(audio_manager_record());
```

Expected state flow:

```text
IDLE
  -> RECORDING
  -> PROCESSING
  -> IDLE
```

The recorded PCM24 samples are processed in place by the existing DSP pipeline.
A successful terminal state sets `recorded_audio_available = true` and updates
`last_samples_recorded`.

## Manual / push-to-talk recording

Manual capture is started and stopped independently:

```c
/* Button press, from task context. */
ESP_ERROR_CHECK(audio_manager_start_recording());

/* Button release, from task context. */
ESP_ERROR_CHECK(audio_manager_stop_recording());
```

The actual retained duration is bounded by whichever happens first:

```text
explicit stop request
        OR
CONFIG_AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS
```

The Kconfig setting defaults to 30 seconds and has a compile-time range of
1..120 seconds. If the caller never sends a stop request, capture auto-stops at
the configured maximum and proceeds through the existing DSP pipeline.

A manual stop request is cooperative. It never disables/deletes I2S from caller
context. The manager observes the request around bounded RX operations and owns
all cleanup.

If a manual tap ends before enough retained samples exist for the DSP minimum,
the short capture is discarded cleanly: the operation returns to `IDLE`,
`last_error` remains `ESP_OK`, and `recorded_audio_available` remains false.

## Shared PSRAM capacity

One manager-owned PCM24 buffer is retained. Its capacity is allocated at init
for the larger of:

```text
config.record_duration_seconds
CONFIG_AUDIO_MANAGER_MANUAL_RECORD_MAX_SECONDS
```

At 16 kHz with PCM24 stored in `int32_t`:

| Duration | Retained PCM24 storage |
| ---: | ---: |
| 5 s | ~312.5 KiB |
| 10 s | ~625 KiB |
| 30 s | ~1.83 MiB |
| 60 s | ~3.66 MiB |

The fixed command still records only `record_duration_seconds`; the larger
capacity does not change its duration.

## Replay retained recording

After a successful fixed/manual record:

```c
ESP_ERROR_CHECK(audio_manager_play_recorded());
```

Expected flow:

```text
IDLE
  -> PLAYBACK
  -> IDLE
```

This reuses the existing recorded PCM24 playback conditioning/gain/compressor/
limiter path. It does not create a second playback algorithm and does not expose
the raw recording buffer publicly.

`audio_manager_play_recorded()` returns `ESP_ERR_INVALID_STATE` when no valid
processed recording is available.

`audio_manager_stop_playback()` now cooperatively cancels either retained-audio
playback or WAV playback. Retained playback observes cancellation between
bounded TX blocks; WAV playback retains its existing cancellation behavior.

## Status and diagnostics

Use `audio_manager_get_status()` or the registered status callback rather than
blocking on an operation:

```c
audio_manager_status_t status = {0};
ESP_ERROR_CHECK(audio_manager_get_status(&status));
```

Recording-specific fields include:

```text
recorded_audio_available
recording_started
recording_completed
recording_failed
recording_manual_stopped
recorded_playback_started
recorded_playback_completed
recorded_playback_failed
recorded_playback_cancelled
last_samples_recorded
last_error
```

Existing RX/TX timeout, overflow, byte-count, and task-stack diagnostics remain
available in the same status snapshot.

## Ownership and conflict rules

Only one operation slot exists. A fixed record, manual record, retained
playback, WAV playback, or golden stability operation owns that slot until its
terminal cleanup completes. Conflicting commands return
`ESP_ERR_INVALID_STATE` instead of building an operation backlog.

Production recording/replay commands are intentionally rejected while the
compile-time Golden Stability Mode is enabled. Golden mode keeps its established
record -> DSP -> recorded-playback ownership and behavior.

Starting a new production recording invalidates the previous retained recording
before the buffer is overwritten. A failed new recording therefore cannot
accidentally replay stale/partially overwritten audio.

## Suggested hardware acceptance

Run these from a task/CLI/test harness while observing `audio_manager_status_t`
and serial diagnostics:

1. Fixed record -> wait for `IDLE` -> verify `recorded_audio_available` -> replay.
2. Manual start -> stop after ~2 s -> wait for processing -> replay.
3. Manual start -> do not stop -> verify auto-stop at configured maximum.
4. Manual start -> immediate stop -> verify clean `IDLE` and no retained audio.
5. Start a second operation while recording -> expect `ESP_ERR_INVALID_STATE`.
6. Replay -> call `audio_manager_stop_playback()` -> verify cooperative cancel.
7. Record -> call `audio_manager_stop()` -> verify bounded RX cleanup and final
   `INITIALIZED` lifecycle state.
8. `start -> record -> stop -> start -> replay/record` lifecycle regression.
9. Compare RX/TX timeout/overflow counters before and after every case.
10. Repeat fixed/manual/replay cycles while checking Internal RAM, DMA RAM,
    PSRAM, and task high-water marks for monotonic leaks.

An ESP-IDF build alone is never hardware validation. Phase 11 target acceptance
has been recorded at closure; repeat the listed checks after changes to MIC
slot detection, I2S RX/TX, DSP output, button timing, or long-run resource
behavior.
