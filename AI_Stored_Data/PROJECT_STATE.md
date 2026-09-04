# Smart Room Cloud Gateway — AI Project State

Updated from branch: `phase/16-audio-arbitration`
Snapshot date: 2026-09-05

## Working Constitution

- `AGENTS.md` is the repository-specific operating guide.
- Preserve completed roadmap history and phase boundaries.
- Inspect implementation and documentation before editing.
- Keep changes evidence-based.
- Never claim build, HIL, merge, or runtime success without evidence.
- `AI_Stored_Data/` is cross-session support metadata only and may be deleted; firmware/build code must never depend on it.

## Current high-level state

```text
Sprint 12  Software complete / HIL PASS
Sprint 13  Software complete / HIL PASS
Sprint 14  Software complete / BUILD PASS / golden-path HIL PASS / targeted regression partial
Sprint 15  Software complete / BUILD VERIFIED / targeted HIL partial
Sprint 16  SOFTWARE COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / BOUNDED HIL ACCEPTED
Major feature-coding stage through Phase 16  COMPLETE
```

Authoritative Phase-16 closure: `AI_Stored_Data/PHASE16_PROGRESS.md`.
Phase-16 HIL plan: `AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md`.
Phase-16 HIL evidence: `AI_Stored_Data/PHASE16_HIL_EVIDENCE.md`.

## HIL routing

```text
RUN PHASE 12 HIL -> test/xiaozhi-p2f-known-audio-e2e
RUN PHASE 13 HIL -> test/phase13-voice-assistant-hil
RUN PHASE 14 HIL -> test/phase14-ptt-voice-e2e-hil
RUN PHASE 15 HIL -> test/phase15-voice-ui-hil
RUN PHASE 16 HIL -> test/phase16-audio-arbitration-hil
```

Inspect the worktree and route to the dedicated test branch before an older-phase HIL run. Never silently use arbitrary latest production source for historical acceptance.

## Established system ownership

- `audio_manager`: sole microphone, speaker, I2S, and DMA owner.
- `xiaozhi_foundation`: sole direct `esp_xiaozhi`/MCP/audio-channel boundary.
- `voice_assistant`: long-lived session/recovery orchestration.
- `voice_assistant_ptt`: PTT authorization policy.
- `voice_assistant_ui_model` plus adapter: copied lifecycle, text, and capture-presentation path.
- `app_gui` / `ui_manager_lvgl`: LVGL owner.
- `sd_card_manager`: SD lifecycle/lease owner.
- GPIO9: factory reset only.
- GPIO38: PTT input, internal pull-down, active-high.
- GPIO48: NeoPixel reservation; never use it as PTT.

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

Phase 14 retains the production Xiaozhi session through unexpected disconnects, protects response ownership by session generation, rejects PTT while a response is awaiting/collecting/finalizing/playing, and bounds response wait to 15 seconds inactivity / 90 seconds total. The recorded golden-path run gives real audible evidence; deferred fault cases and fresh audible evidence on the regression image remain pending.

Phase 15 adds copied lifecycle plus USER/ASSISTANT text presentation through the UI task. The UI reports `RECORDING` only when actual microphone capture is active, renders a live `RECORD` duration during capture, and freezes the duration afterward. Exact CONNECTING/THINKING/RECOVERING labels remain intentionally coalesced by the reused legacy GUI surface. The visible UI/text behavior still requires HIL evidence.

## Phase 16 — audio arbitration architecture

Logical audio clients request resources through project-owned metadata:

```text
audio_manager_request_t
├── request_id
├── client
├── resource = CAPTURE / PLAYBACK
├── priority
├── busy_policy = REJECT / QUEUE / PREEMPT_LOWER_PRIORITY
└── interruptible
```

Known clients include SYSTEM, XIAOZHI, NOTIFICATION, ALARM, RECORDER, UI, and TEST. No client receives an I2S handle, DMA buffer, raw source handle, or direct hardware ownership.

### Playback

```text
client
-> playback arbiter
-> public audio_manager playback control
-> manager task
-> sole I2S TX
```

One current and one pending WAV request are bounded. ACTIVE requires real PLAYBACK evidence, not command acceptance. A known interruptible lower-priority playback may be cooperatively stopped through `audio_manager_stop_playback()`.

### Capture

```text
client
-> capture arbiter
-> audio_manager_start_recording()
-> manager task
-> sole I2S RX
```

One current and one pending request are bounded. ACTIVE requires real RECORDING evidence. PROCESSING is allowed to finish naturally before pending promotion. Cooperative stop uses `audio_manager_stop_recording()`.

### Deterministic policy

```text
no known owner -> GRANT
REJECT -> REJECT
QUEUE -> WAIT
PREEMPT_LOWER_PRIORITY -> PREEMPT only when incoming priority is strictly higher and current owner is interruptible
same/lower priority -> never preempt
```

Unknown legacy/external manager activity is never preempted because trusted client metadata is absent. Capture and playback arbiters are separate but share the one `audio_manager` operation state; manager serialization remains the final hardware gate. Global cross-resource fairness is intentionally not claimed.

### Xiaozhi, notification, and alarm policy

```text
XIAOZHI      priority=70  CAPTURE=REJECT  PLAYBACK=QUEUE  interruptible=true
NOTIFICATION priority=50  PLAYBACK=QUEUE  interruptible=true
ALARM        priority=100 PLAYBACK=PREEMPT_LOWER_PRIORITY interruptible=false
```

The Phase-14 downlink still waits for manager IDLE before requesting Xiaozhi playback, so queue-behind-current-playback is not yet used at that call site. This is accepted MVP behavior with Phase-16 HIL accepted; streaming optimization remains deferred.

## Known pending acceptance / technical debt

1. Phase-16 bounded target HIL is accepted; endurance and full integration regression remain deferred.
2. Phase-15 visible LCD/text HIL remains partial: `RECORDING` timer, USER/ASSISTANT text, latest-turn behavior, recovery presentation, truncation, and UI-resource evidence need target confirmation.
3. Phase-14 fault-injection cases remain deferred; the exact regression image still needs fresh audible speaker confirmation.
4. Phase-14 response playback remains PSRAM/SD-backed rather than low-latency streaming.
5. Long-duration Firebase/cloud plus Xiaozhi simultaneous-traffic regression remains deferred.
6. Playback/capture arbiters have no dedicated stop/deinit lifecycle API yet; recorded-audio playback is not migrated to arbitration.
7. Notification/alarm priority/preemption passed the bounded target matrix; repeated timing/endurance coverage remains deferred.

## Next-work guidance

1. Retain the Phase-16 HIL branch as a regression baseline.
2. Finish the still-pending Phase-15 visible UI/text acceptance.
3. Treat Phase 12 and Phase 13 as closed regression baselines; retain Phase 14's recorded golden-path result and run only relevant regressions.
4. Do not start Phase 17 automatically.
5. After independent acceptance, enter full Gateway/Firebase integration regression, bug fixing, hardening, performance/resource validation, documentation, and release/portfolio closure.
