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
Sprint 16  IN PROGRESS / 16-C complete / 16-D next
```

Authoritative Phase-16 progress: `AI_Stored_Data/PHASE16_PROGRESS.md`.

## Deferred HIL routing

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil   # verify/create prerequisite
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
```

Codex may receive these commands while standing on the latest branch, but must inspect git status and checkout the dedicated test branch before running HIL. Never silently test an older phase with Phase-16 production source.

## Established ownership through Phase 15

- `audio_manager`: sole microphone/speaker/I2S/DMA owner.
- `xiaozhi_foundation`: sole direct `esp_xiaozhi`/MCP/audio-channel boundary.
- `voice_assistant`: long-lived session/recovery orchestration.
- `voice_assistant_ptt`: PTT authorization policy.
- `app_gui` / `ui_manager_lvgl`: LVGL owner.
- `sd_card_manager`: SD lifecycle/lease owner.
- GPIO9: factory reset only.
- GPIO5: temporary Phase-14 PTT input, internal pull-down, active-high.

Phase-14 voice path remains PTT -> INMP441 -> copied PCM16 -> Xiaozhi uplink -> response -> SD-backed WAV -> `audio_manager` -> MAX98357. Phase-15 adds copied lifecycle/USER/ASSISTANT presentation through `voice_assistant_ui_model` -> GUI adapter -> `app_gui` UI task.

## Phase 16 — Audio Arbitration & Multi-Client Audio Policy

Reason: sole I2S ownership prevents hardware conflicts but does not decide policy between legitimate clients such as Xiaozhi, notifications, alarms or another recorder.

### 16-A — request model complete

`audio_manager_request_t` contains request ID, logical client, CAPTURE/PLAYBACK resource, priority, `REJECT/QUEUE/PREEMPT_LOWER_PRIORITY` busy policy and interruptibility.

### 16-B — playback arbitration runtime complete

```text
client
-> audio_manager_playback_arbiter
-> public audio_manager playback control
-> audio_manager task
-> sole I2S TX owner
```

The runtime stores one current + one pending WAV request, uses real PLAYBACK evidence before ACTIVE, and preempts only known interruptible lower-priority arbiter-owned playback through cooperative `audio_manager_stop_playback()`.

### 16-C — capture arbitration runtime complete

```text
client
-> audio_manager_capture_arbiter
-> audio_manager_start_recording()
-> audio_manager task
-> sole I2S RX owner
```

Capture state:

```text
IDLE -> STARTING -> ACTIVE(RECORDING) -> FINISHING(PROCESSING) -> IDLE
                         |
                         -> PREEMPTING via cooperative stop
```

The capture runtime stores one current + one pending request. Cancel/preempt before actual RECORDING does not touch hardware. Preemption during RECORDING requests cooperative stop. Once manager enters PROCESSING, arbiter waits for natural DSP cleanup/IDLE before promoting pending work.

Unknown legacy Phase-14 capture/playback remains external busy and is never preempted until trusted client migration occurs in 16-E.

### Production startup

```text
audio_manager READY
-> playback arbiter READY
-> capture arbiter READY
-> voice/UI/PTT/uplink/downlink stack
```

## Current Phase-16 policy gap

Playback and capture arbiters now exist separately, but `audio_manager` still has one shared operation state. 16-D must define deterministic cross-resource behavior when capture and playback requests compete, plus fairness/equal-priority/pending replacement/cancellation diagnostics.

No new client receives I2S/DMA/file/raw-buffer ownership.

## Cross-system concurrency rule

Sensor, Firebase/cloud, GUI and normal FreeRTOS tasks may continue while voice/audio runs. Do not globally pause unrelated work as a substitute for resource arbitration.

```text
CPU/task interleaving                       allowed
Firebase + Xiaozhi network coexistence      allowed; measure on HIL
multiple direct I2S owners                  forbidden
multiple legitimate audio-manager clients  centrally arbitrated
```

## Known acceptance/build risks

1. Phase-14/15 target build/HIL still pending.
2. Actual Xiaozhi response codec still needs target proof; Opus evidence requires decoder work.
3. Phase-14 downlink remains PSRAM/SD-backed rather than low-latency streaming.
4. GPIO5 is temporary.
5. Firebase + Xiaozhi simultaneous load remains unmeasured.
6. Phase-15 semantic bridge needs real build evidence against pinned `esp_xiaozhi` 0.1.2.
7. Phase-16 playback/capture arbiters have static review only; no build/HIL evidence.
8. Arbiter stop/deinit lifecycle APIs are not implemented yet.
9. Xiaozhi/notification/alarm clients are not migrated until 16-E.

## Next-work guidance

When asked **"hiện tại nên làm gì tiếp theo?"**:

1. surface Phase-12/13 deferred Codex-ready HIL;
2. mention Phase-14 dedicated test-branch prerequisite;
3. mention Phase-15 test branch/HIL deferred;
4. Phase 16 is active: 16-A, 16-B and 16-C complete;
5. next software checkpoint is **16-D — Priority / Preemption / Queue Policy Hardening**, only after explicit `tiếp tục`.

When hardware returns, recommended acceptance order remains Phase 12 -> Phase 13 -> Phase 14 -> Phase 15 -> later Phase-16 arbitration HIL -> full Gateway/Firebase regression using production history only.
