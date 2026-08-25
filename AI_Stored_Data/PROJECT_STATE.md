# Smart Room Cloud Gateway — AI Project State

Updated from branch: `phase/16-audio-arbitration`
Snapshot date: 2026-08-25

## Working Constitution

- `AGENTS.md` is the repository-specific operating guide.
- Preserve completed roadmap history and phase boundaries.
- Inspect implementation/docs before editing.
- Keep changes evidence-based.
- Never claim build/HIL/merge/runtime success without evidence.
- `AI_Stored_Data/` is cross-session support metadata only and may be deleted; firmware/build code must never depend on it.

## Current high-level state

```text
Sprint 12  Software complete / selected HIL deferred
Sprint 13  Software complete / HIL deferred
Sprint 14  Software complete / Build + HIL pending
Sprint 15  Software complete / Build + HIL pending
Sprint 16  SOFTWARE COMPLETE / STATIC REVIEW COMPLETE / Build + HIL pending
Major feature-coding stage through Phase 16  COMPLETE
```

Authoritative Phase-16 closure: `AI_Stored_Data/PHASE16_PROGRESS.md`.
Phase-16 HIL plan: `AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md`.

## Deferred HIL routing

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil   # verify/create prerequisite
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL -> test/phase16-audio-arbitration-hil   # recommended branch, create/populate before execution
```

Codex may receive HIL commands while standing on the latest branch, but must inspect git status and route to the dedicated test branch before executing an older-phase HIL. Never silently test older acceptance against arbitrary latest production source.

## Established system ownership

- `audio_manager`: sole microphone/speaker/I2S/DMA owner.
- `xiaozhi_foundation`: sole direct `esp_xiaozhi`/MCP/audio-channel boundary.
- `voice_assistant`: long-lived session/recovery orchestration.
- `voice_assistant_ptt`: PTT authorization policy.
- `voice_assistant_ui_model` + adapter: copied voice presentation path.
- `app_gui` / `ui_manager_lvgl`: LVGL owner.
- `sd_card_manager`: SD lifecycle/lease owner.
- GPIO9: factory reset only.
- GPIO5: temporary PTT input, internal pull-down, active-high.

## Phase 14/15 voice path

```text
PTT
-> Xiaozhi READY
-> INMP441 / audio_manager capture
-> copied PCM16 uplink
-> Xiaozhi response
-> bounded downlink aggregation
-> SD-backed WAV
-> audio_manager playback
-> MAX98357
```

Phase 15 adds copied lifecycle + USER/ASSISTANT text presentation through the UI task. Exact backend state remains richer than the reused legacy GUI enum for CONNECTING/THINKING/RECOVERING.

## Phase 16 — final audio arbitration architecture

Logical audio clients now request resources through project-owned metadata:

```text
audio_manager_request_t
├── request_id
├── client
├── resource = CAPTURE / PLAYBACK
├── priority
├── busy_policy = REJECT / QUEUE / PREEMPT_LOWER_PRIORITY
└── interruptible
```

Known clients include SYSTEM, XIAOZHI, NOTIFICATION, ALARM, RECORDER, UI and TEST.

### Playback

```text
client
-> playback arbiter
-> public audio_manager playback control
-> manager task
-> sole I2S TX
```

One current + one pending WAV request only. ACTIVE requires real PLAYBACK evidence, not command acceptance. Known interruptible lower-priority playback can be cooperatively stopped through `audio_manager_stop_playback()`.

### Capture

```text
client
-> capture arbiter
-> audio_manager_start_recording()
-> manager task
-> sole I2S RX
```

One current + one pending request only. ACTIVE requires real RECORDING evidence. PROCESSING is allowed to finish naturally before pending promotion. Cooperative stop uses `audio_manager_stop_recording()`.

### Deterministic policy

```text
no known owner -> GRANT
REJECT -> REJECT
QUEUE -> WAIT
PREEMPT_LOWER_PRIORITY -> PREEMPT only when incoming priority is strictly higher and current owner is interruptible
same/lower priority -> never preempt
```

Unknown legacy/external manager activity is never preempted because trusted client metadata is absent.

### Xiaozhi integration

Capture:

```text
XIAOZHI priority=70
CAPTURE
busy_policy=REJECT
interruptible=true
```

Playback:

```text
XIAOZHI priority=70
PLAYBACK
busy_policy=QUEUE
interruptible=true
```

The Phase-14 downlink still waits for manager IDLE before submitting Xiaozhi playback, so queue-behind-current-playback is not exploited at that call site. This is accepted MVP behavior and should be optimized only after HIL evidence.

### Notification / alarm helpers

```text
NOTIFICATION priority=50 QUEUE interruptible=true
ALARM        priority=100 PREEMPT_LOWER_PRIORITY interruptible=false
```

A critical alarm may cooperatively preempt active Xiaozhi playback. A notification may not preempt Xiaozhi and only uses the bounded pending slot if available.

## Cross-resource rule

Capture and playback arbiters are separate but share one `audio_manager` operation state. Only manager IDLE permits a new hardware operation. If both race for IDLE, manager serialization remains the final hardware gate. Hardware safety is preserved; global cross-resource fairness is not guaranteed.

If HIL proves sustained starvation, the evidence-based next architecture is a unified audio scheduler. Do not introduce unbounded fairness queues preemptively.

## Concurrency rule

Sensor, Firebase/cloud, Wi-Fi, GUI, SD, performance monitor and Xiaozhi transport tasks may continue during voice/audio. Phase 16 arbitrates audio resources only.

Do not pause unrelated subsystems as a substitute for ownership/arbitration without target evidence.

## Known pending acceptance / technical debt

1. Phase-14/15/16 target build/HIL still pending.
2. Actual Xiaozhi response codec must still be proven; Opus requires decoder work.
3. Phase-14 downlink remains PSRAM/SD-backed rather than low-latency streaming.
4. GPIO5 PTT is temporary.
5. Firebase + Xiaozhi simultaneous load remains unmeasured.
6. Phase-15 semantic bridge and Phase-16 source-local CMake bridges require real build evidence.
7. playback/capture arbiters have no dedicated stop/deinit lifecycle API yet.
8. global cross-resource fairness is not guaranteed.
9. recorded-audio playback is not migrated to arbitration.
10. Phase-14 Xiaozhi downlink pre-waits for IDLE before arbitration submit.
11. notification/alarm policy needs target instrumentation/HIL evidence.

## Next-work guidance

When asked **"hiện tại nên làm gì tiếp theo?"**:

1. surface deferred HIL Phase 12 first;
2. Phase 13 HIL;
3. Phase 14 PTT voice HIL/test-branch prerequisite;
4. Phase 15 voice UI HIL;
5. Phase 16 audio-arbitration HIL plan/test-branch prerequisite;
6. do **not** automatically start Phase 17;
7. after independent acceptance, move into full Gateway/Firebase integration regression, bug fixing, hardening, performance/resource validation, documentation and release/portfolio closure.

Phase 16 is the end of the planned **major feature-coding stage**. Future coding should primarily be evidence-driven fixes/hardening unless Hải explicitly expands product scope.
