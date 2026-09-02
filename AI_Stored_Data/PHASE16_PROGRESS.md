# Phase 16 — Audio Arbitration & Multi-Client Audio Policy

Updated: 2026-09-02
Branch: `phase/16-audio-arbitration`
Base branch: `phase/15-voice-assistant-ui`
Current checkpoint: **16-F — FINAL REVIEW / CLOSURE**
Status: **SOFTWARE COMPLETE / STATIC REVIEW COMPLETE / BUILD VERIFIED / HIL PENDING**

## Collaboration result

All planned Phase-16 checkpoints are complete:

1. 16-A — audio client/request model. ✅
2. 16-B — playback arbitration runtime. ✅
3. 16-C — capture arbitration runtime. ✅
4. 16-D — priority/preemption/queue policy hardening. ✅
5. 16-E — Xiaozhi + notification/alarm integration/stubs and concurrency review. ✅
6. 16-F — final review/docs/deferred HIL. ✅

Phase 16 is the planned end of the project's **major feature-coding stage**. Do not start Phase 17 automatically. Subsequent work should primarily be build verification, HIL, integration, bug fixing, hardening, performance/resource validation, documentation and release/portfolio closure unless Hải explicitly adds new feature scope.

## Final Phase-16 architecture

`audio_manager` remains the sole I2S/DMA owner.

```text
logical clients
├── XIAOZHI
├── NOTIFICATION
├── ALARM
├── SYSTEM
├── RECORDER
├── UI
└── TEST
        ↓
request metadata
(client/resource/priority/busy policy/interruptibility/request ID)
        ↓
CAPTURE arbiter / PLAYBACK arbiter
        ↓
existing public audio_manager controls
        ↓
audio_manager task
        ↓
sole I2S RX/TX + DMA ownership
```

No client receives an I2S handle, DMA buffer, raw source handle or direct hardware ownership.

## Playback arbitration

Bounded storage:

```text
1 current WAV request
1 pending WAV request
```

Runtime state:

```text
IDLE -> STARTING -> ACTIVE -> IDLE
                    |
                    -> PREEMPTING -> IDLE
ERROR is observable
```

`audio_manager_play_wav() == ESP_OK` means command accepted only. ACTIVE is entered only after copied manager status reports real PLAYBACK evidence. Completion is recognized only after PLAYBACK -> IDLE.

Preemption is cooperative through `audio_manager_stop_playback()`.

## Capture arbitration

Bounded storage:

```text
1 current capture request
1 pending capture request
```

Runtime state:

```text
IDLE -> STARTING -> ACTIVE(RECORDING) -> FINISHING(PROCESSING) -> IDLE
                         |
                         -> PREEMPTING via cooperative stop
```

`audio_manager_start_recording() == ESP_OK` means command accepted only. ACTIVE requires real RECORDING evidence. Once the manager moves to PROCESSING, Phase 16 does not try to stop DSP; it waits for manager-owned cleanup/IDLE.

## Deterministic policy

Shared decision contract:

```text
GRANT / WAIT / REJECT / PREEMPT
```

Rules:

```text
no known owner -> GRANT
REJECT policy -> REJECT
QUEUE policy -> WAIT
PREEMPT_LOWER_PRIORITY -> PREEMPT only when:
    incoming.priority > current.priority
    AND current.interruptible == true
same priority -> never preempt
lower priority -> never preempt
```

Equal priority therefore cannot create preemption ping-pong.

Each resource has one pending slot only. An occupied pending slot is not silently replaced. Additional queued work is rejected deterministically.

## Cross-resource behavior

Capture and playback arbiters are structurally separate but sit above one `audio_manager` operation state.

```text
manager != IDLE
-> no new hardware operation may start

manager == IDLE
-> an arbiter may submit
```

If capture and playback race for IDLE, the existing manager command serialization is the final hardware gate. One wins; the other receives busy/invalid-state behavior and waits/retries according to its arbiter state.

This is safe for hardware ownership, but **global fairness between the two independent arbiter tasks is not guaranteed**. Do not claim fairness. If HIL proves sustained starvation, the evidence-based next architecture is a unified audio scheduler above both resources.

## Xiaozhi integration

### Capture

```text
PTT authorized
-> XIAOZHI CAPTURE request
   priority=70
   busy_policy=REJECT
   interruptible=true
-> capture arbiter
-> manager RECORDING evidence
-> existing Phase-14 uplink continues
```

PTT capture intentionally rejects instead of queueing when the microphone is unavailable so capture cannot start late after the user has already spoken.

### Playback

```text
response WAV ready
-> XIAOZHI PLAYBACK request
   priority=70
   busy_policy=QUEUE
   interruptible=true
-> playback arbiter
-> real manager PLAYBACK evidence
-> existing Phase-14 completion wait continues
```

Important closure nuance: the Phase-14 downlink still calls `downlink_wait_audio_idle()` before requesting Xiaozhi playback. Therefore the current production Xiaozhi call site does **not** exploit queue-behind-current-playback; it waits for manager IDLE before submit. This is accepted for the MVP because it preserves existing semantics. Alarm preemption still applies once Xiaozhi playback is ACTIVE. Optimization/removal of this pre-wait belongs after HIL evidence, not in closure churn.

## Named notification/alarm integration

Minimal policy helpers exist:

```c
audio_manager_play_notification_wav(request_id, path);
audio_manager_play_critical_alarm_wav(request_id, path);
```

Defaults:

```text
NOTIFICATION priority=50 QUEUE interruptible=true
ALARM        priority=100 PREEMPT_LOWER_PRIORITY interruptible=false
```

Expected behavior:

```text
Xiaozhi SPEAKING + Notification
-> notification cannot preempt Xiaozhi; bounded queue if available

Xiaozhi SPEAKING + Critical Alarm
-> cooperative Xiaozhi stop
-> alarm promoted after manager returns IDLE

Critical Alarm active + Xiaozhi response
-> Xiaozhi cannot preempt alarm
```

These are arbitration integration surfaces only; Phase 16 does not add a new user-facing alarm product feature.

## Concurrency conclusion

Phase 16 arbitrates **audio resources only**. It does not pause unrelated tasks.

Allowed concurrent work remains:

```text
sensor tasks
Firebase/cloud tasks
Wi-Fi/event tasks
GUI/LVGL task
SD manager/recovery
performance monitor
Xiaozhi transport tasks
```

Contention in CPU/network/heap must be measured in full integration HIL. Do not add hidden `PTT -> pause Firebase` behavior without evidence.

## Static-review defects fixed before closure

1. fixed false playback completion/start assumptions by separating command acceptance from real PLAYBACK evidence;
2. fixed capture command-acceptance vs real RECORDING evidence;
3. fixed pre-start cancel/preempt edge cases in capture/playback ownership;
4. protected unknown legacy/external operations from untrusted preemption;
5. fixed a real compile blocker in the Phase-16 policy header: it referenced nonexistent `audio_manager_request.h`; it now includes `audio_manager_arbitration.h`;
6. preserved Phase-14 Xiaozhi timing semantics through a source-local arbitration bridge that waits for real manager state evidence before returning start success.

## Remaining accepted technical debt / risks

These do not reopen Phase 16 software scope by themselves:

1. no target HIL evidence yet;
2. playback/capture arbiters have init/start but no dedicated stop/deinit lifecycle API;
3. cross-resource global fairness is not guaranteed;
4. recorded-audio playback is not migrated into arbitration;
5. source-local CMake compatibility redirects are a maintainability seam; their Phase-15/16 link integration is build verified but remains a future maintenance boundary;
6. Xiaozhi downlink pre-waits for IDLE before playback submit, limiting queue usefulness at that call site;
7. Phase-14 codec/SD-backed playback risks remain separate dependencies;
8. notification/alarm helpers require HIL instrumentation/use to prove priority/preemption timing;
9. simultaneous Firebase/cloud + Xiaozhi + arbitration load remains unmeasured.

## Phase-16 HIL

Plan: `AI_Stored_Data/PHASE16_HIL_TEST_PLAN.md`

Recommended test branch:

```text
test/phase16-audio-arbitration-hil
```

Activation label:

```text
RUN PHASE 16 HIL
```

The plan covers build/link, boot regression, real Xiaozhi capture/playback ownership, notification queueing, alarm preemption, equal-priority protection, bounded queue pressure, capture contention, cross-resource race, cancellation edge cases and full Gateway coexistence/resource trend.

No HIL PASS is claimed until real target evidence satisfies the plan.

## Closure

Phase 16 is closed as:

```text
Software implementation       COMPLETE
Static review                 COMPLETE
ESP-IDF build verification    PASS (ESP-IDF 6.0.1; binary 0x21e7b0; 47% app free)
Hardware acceptance           PENDING
Full integration regression   PENDING
```

This closes the planned **major feature-coding stage** through Phase 16. The next project stage should be acceptance/integration/hardening rather than opening another major feature phase automatically.
