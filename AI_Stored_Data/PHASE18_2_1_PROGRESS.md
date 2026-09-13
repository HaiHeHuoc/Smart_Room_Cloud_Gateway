# Phase 18.2.1 - Audio Playback Control Progress

Updated: 2026-09-13

```text
Phase status:       IN PROGRESS / SOFTWARE IMPLEMENTED
Prompt 2 core:      SOFTWARE IMPLEMENTED / BUILD VERIFIED
Prompt 3 MCP/PTT:   SOFTWARE IMPLEMENTED / BUILD + HOST TESTS VERIFIED
Target HIL:         PENDING / NOT CLAIMED
```

## Git checkpoint

```text
base branch:            main_including_Firebase_security
base commit:            f2597fd8fc60be54a66dc75dc6a7f718b9901c88
implementation branch:  phase/18.2.1-playback-control-core
implementation commit:  c7044f1c26b3d203aa022fac8ec785969a8ecc63
Prompt-2 final HEAD:     69e5da649e0a25801df752a5e8a1ebc25e3520c6
Prompt-3 branch:         phase/18.2.1-mcp-ptt-integration
Prompt-3 implementation: e1b841bde21ae5c379c68a51ae5cf82f0c9cd932
```

This branch must not be merged until Prompt 3 review and target acceptance are
explicitly handled.

## Prompt 3 integration

Production MCP now exposes exactly:

```text
audio.control_playback { action: pause | resume | stop | restart }
audio.get_playback_state {}
```

The SDK-generated input schema makes `action` required and rejects additional
properties. The control result reports `success`, `accepted`,
`physically_applied`, deterministic `error_code`, and a bounded copied playback
snapshot. The state tool is annotated read-only and exposes only state, source
type, pause reason, resumability, generation, committed/total frame position,
and position granularity. Neither tool exposes a path, FILE, handle, pointer,
raw PCM, I2S, DMA, or SD ownership object.

The ownership flow is:

```text
xiaozhi_foundation schema/tool
-> smart_room_mcp_adapter enum adaptation
-> voice_assistant per-turn policy
-> audio_manager public control API
-> sole source/I2S/DMA owner
```

GPIO38 uses the same project-owned control seam locally. Playing resumable
audio is paused with `PTT`, and the PTT task waits finitely for logical PAUSED,
manager IDLE, and inactive playback I2S before authorizing the same held press.
A release queued during the wait revokes the unstarted turn and restores the
source. A prior USER pause is preserved and is never auto-resumed by an
unrelated question.

After downlink reaches terminal playback, timeout, or cooperative abort, the
generation-bound transaction applies exactly one final action. No explicit MCP
action means temporary PTT audio auto-resumes. The last accepted explicit
pause/resume/stop/restart action replaces auto-resume and is applied after TTS,
so local audio does not overlap the spoken acknowledgement.

A GPIO38 press during Xiaozhi speech transfers any still-valid local suspended
context to the new PTT generation, then asks the downlink owner to taint old
PCM, terminate the arbiter stream, close/reset the audio channel, clear queued
response state, and only then continue the retained press. Old TTS is never
resumed.

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

Prompt 3 commands:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\application\voice_assistant\test\host\run_tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File components\application\xiaozhi_foundation\test\host\run_tests.ps1
```

Result: PASS.

- PTT suspension needed/not-needed, USER pause preservation, temporary
  auto-resume, all four explicit overrides, last-command-wins, fast release,
  stale terminal rejection, and TTS-generation transfer: PASS;
- exact MCP action allowlist, invalid/missing/case-mismatched actions, read-only
  state annotation, and provider-boundary static guard: PASS.

### ESP-IDF build

ESP-IDF 6.0.1:

```powershell
idf.py reconfigure
ninja -C build -j 1 all
```

Prompt 3 final result: PASS. Firmware size `0x26c110`; `0x193ef0` bytes (39%)
remain in the 4 MiB app partition.

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

## Prompt 3 target HIL matrix

1. WAV playing -> GPIO38 hold -> ask temperature -> hear TTS -> resume near the
   retained committed position.
2. Temporary suspension plus STOP -> remain IDLE after acknowledgement.
3. Temporary suspension plus PAUSE -> remain PAUSED with USER reason.
4. USER-paused source plus RESUME -> resume only after TTS completes.
5. Playing source plus RESTART -> same source starts at frame zero after TTS.
6. Fast GPIO38 tap during suspension -> no late microphone start; source
   recovers.
7. GPIO38 during Xiaozhi TTS -> old TTS terminates, same press starts a new turn,
   and old speech never resumes.
8. Repeat 20-50 suspend/voice/resume cycles, rapid GPIO38, network failure, and
   SD-unavailable-before-resume; verify a subsequent clean turn/playback.

Capture equivalent-checkpoint Internal/DMA/PSRAM free/min/largest, audio/PTT/
uplink/downlink/reader stack HWM, CPU, pause/authorization/resume latency, and SD
lease/resource trend. No target values are claimed yet.

## Remaining work

Run the Prompt 2 + Prompt 3 target HIL matrix and record audible, GPIO38,
resource, and failure-recovery evidence. Until then:

```text
PHASE 18.2.1 READY TO CLOSE: NO
READY TO PLAN PHASE 18.2.2: NO
```

Do not start Phase 18.2.2 automatically. Phase 18.2.1 remains IN PROGRESS until
target acceptance is recorded.
