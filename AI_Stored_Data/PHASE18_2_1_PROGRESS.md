# Phase 18.2.1 - Audio Playback Control Progress

Updated: 2026-09-13

```text
Phase status:       IN PROGRESS
Prompt 2 core:      SOFTWARE IMPLEMENTED / BUILD VERIFIED
Prompt 3 MCP/PTT:   NOT STARTED
Target HIL:         PENDING
```

## Git checkpoint

```text
base branch:            main_including_Firebase_security
base commit:            f2597fd8fc60be54a66dc75dc6a7f718b9901c88
implementation branch:  phase/18.2.1-playback-control-core
implementation commit:  pending at the time this record was introduced
```

The implementation commit is recorded by the follow-up documentation commit
after the code checkpoint exists. This branch must not be merged until Prompt
2 review and later target acceptance are explicitly handled.

## Implemented owner capability

- Public task-context controls: cooperative pause with USER/PTT reason,
  generation-guarded resume, restart of the same source, idempotent stop, and a
  copied playback status.
- Public state: `IDLE`, `STARTING`, `PLAYING`, `PAUSING`, `PAUSED`, `RESUMING`,
  `STOPPING`, and transient `ERROR` before stable `IDLE` recovery.
- Copied status includes bounded source kind, resumability, generation,
  committed/total mono frames, 256-frame position granularity, pause reason,
  last action, and last control result. It exposes no path, pointer, PCM data,
  FILE, I2S handle, or DMA address.
- WAV pause stops TX, joins/destroys the reader, closes FILE, releases its SD
  lease, and frees prefetch PSRAM/queues/events before publishing `PAUSED`.
  Resume fresh-opens the same copied path, verifies full WAV metadata, and
  seeks to the committed data offset.
- Retained-recording pause stops TX and retains only the existing PCM24 buffer
  plus a committed sample/frame index; no duplicate recording buffer exists.
- Live PCM16/Xiaozhi remains non-seekable: pause/resume/restart return
  `ESP_ERR_NOT_SUPPORTED`; cooperative stop/cancel is unchanged.
- An arbiter-owned WAV remains the current request with request/arbiter state
  `PAUSED`; its single pending request is not promoted during suspension.

## State and acceptance semantics

```text
IDLE -> STARTING -> PLAYING -> PAUSING -> PAUSED
                         ^                    |
                         +---- RESUMING <-----+
PLAYING/PAUSED -> STOPPING -> IDLE
PLAYING -> RESTART -> STARTING -> PLAYING at frame 0
PAUSED  -> RESTART -> RESUMING -> PLAYING at frame 0
failure -> ERROR -> stable IDLE with manager last_error retained
```

`ESP_OK` from pause/resume/restart/stop means request accepted. Physical pause
is proven only by copied `PAUSED`; physical resume/restart is proven only by
copied `PLAYING`. A non-zero resume generation rejects stale PTT/policy work;
zero explicitly targets the current paused context.

The position is the last complete 256-frame mono block submitted to I2S. It is
not a sample-perfect or exact audible cursor. At 16 kHz the normal tolerance is
one 16 ms block plus driver/DMA buffering.

## Source matrix

| Source | Pause | Resume | Restart | Stop |
| --- | --- | --- | --- | --- |
| Local WAV | Yes, cooperative | Same copied source, fresh verify/seek | Same source at frame 0 | Yes; context/resources cleared |
| Retained recording | Yes, cooperative | Existing PCM24 at committed frame | Existing recording at frame 0 | Yes; context cleared |
| Xiaozhi PCM16/TTS | Not supported | Not supported | Not supported | Existing cooperative abort/cancel |
| Notification/alarm policy | Unchanged/out of scope | Out of scope | Out of scope | Existing Phase-16 policy unchanged |

## Files and ownership

- `audio_manager.h` defines only project-owned control/status types and APIs.
- `audio_manager.c` retains physical state, source identity, generation,
  position, resource cleanup, and the sole I2S/DMA ownership.
- playback-control policy is a private pure transition helper; it owns no task
  or hardware.
- WAV prefetch accepts a private verified start offset; its worker remains sole
  owner of FILE and the SD lease while active.
- playback arbiter reflects suspended WAV ownership without redesigning the
  one-current/one-pending Phase-16 policy.

## Validation performed

### Static review

- Scope/ownership review against Prompt-1 approval: PASS.
- No MCP registration, GPIO38/PTT behavior, Phase 18.2.2, 18.3, or 18.4 source
  change: PASS.
- `git diff --check`: PASS.
- Firebase configuration mapping remained `CONFIG_APP_FIREBASE_*`; no literal
  credentials were introduced.

### Host/unit tests

Command:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\audio\audio_manager\test\host\run_tests.ps1
```

Result: PASS.

- playback transition matrix: PASS;
- committed WAV seek, invalid bounds/alignment, and single SD-lease release:
  PASS.

These tests do not emulate FreeRTOS tasks, I2S, PSRAM, real SD latency, or
audible output.

### ESP-IDF build

ESP-IDF 6.0.1:

```powershell
idf.py reconfigure
ninja -C build -j 1 all
```

Result: PASS. Firmware size `0x268ff0`; `0x197010` bytes (40%) remain in the
4 MiB app partition.

### Target HIL

NOT RUN / PENDING. No audible, SD, I2S/MAX98357A, repeated-cycle, or hardware
resource-stability PASS is claimed by Prompt 2.

## Pending Prompt-2 HIL matrix

1. WAV PLAY -> PAUSE -> RESUME.
2. WAV pause at several positions -> continuation within documented tolerance.
3. WAV PLAY -> RESTART.
4. WAV PAUSE -> RESTART.
5. WAV PLAY -> STOP.
6. WAV PAUSE -> STOP.
7. Repeated PAUSE.
8. Repeated STOP.
9. Invalid RESUME from IDLE.
10. Pause near EOF.
11. Pause during prefetch/refill.
12. Remove/unavailable SD before RESUME.
13. Resume after SD recovery if the current remount policy permits it.
14. Repeat 20-50 pause/resume cycles.
15. Start fresh playback after every stop/error case.

For equivalent workload checkpoints capture Internal free/min/largest, DMA
free/min/largest, PSRAM free/largest, manager/reader stack HWM, CPU, SD lease
count, pause/resume latency, and monotonic heap/resource-loss trend.

## Remaining risks

- Cooperative response is bounded by the current I2S write timeout and by the
  return of a synchronous SD/VFS read; a wedged stdio read cannot be forcibly
  preempted safely.
- Target HIL must verify actual listener continuity and quantify the committed
  block plus DMA audible tolerance.
- SD absent at fresh resume currently fails that operation deterministically;
  Prompt 2 does not add an indefinite media wait.
- Retained-recording and WAV control share the physical manager owner, while
  only arbiter-submitted WAV has a Phase-16 logical arbiter request. No new
  recorded-playback start surface was added because that belongs to 18.2.2.

## Next work

Phase 18.2.1 Prompt 3 only, after Prompt 2 review/HIL direction:

```text
audio.control_playback MCP
audio.get_playback_state MCP
GPIO38 local suspension
same retained physical press into PTT
fast-release capture revocation/immediate local resume
post-turn generation-guarded auto-resume
explicit audio-command override
```

Do not start Prompt 3 automatically. Phase 18.2.1 remains IN PROGRESS.
