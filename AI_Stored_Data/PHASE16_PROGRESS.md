# Phase 16 — Audio Arbitration & Multi-Client Audio Policy

Updated: 2026-08-25
Branch: `phase/16-audio-arbitration`
Base branch: `phase/15-voice-assistant-ui`
Current checkpoint: **16-E — Xiaozhi + Notification/Alarm Integration & Concurrency Review**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 16 follows the same checkpoint workflow as Phases 13-15. Implement one checkpoint, stop, explain the result, and continue only after Hải explicitly says `tiếp tục`.

Planned checkpoints:

1. 16-A — audio client/request model. ✅
2. 16-B — playback arbitration runtime. ✅
3. 16-C — capture arbitration runtime. ✅
4. 16-D — priority/preemption/queue policy hardening. ✅
5. 16-E — Xiaozhi + notification/alarm integration/stubs and concurrency review. ✅
6. 16-F — final review/docs/deferred HIL. NEXT / FINAL PHASE-16 PROMPT

At 16-F explicitly notify Hải that it is the final Phase-16 prompt before software closure.

## Why Phase 16 exists

`audio_manager` remains the sole I2S/DMA owner. Phase 16 adds bounded policy for multiple legitimate CAPTURE/PLAYBACK clients without exposing hardware ownership.

## 16-A through 16-D summary

- project-owned request metadata: client/resource/priority/busy policy/interruptibility/request ID;
- bounded playback arbiter: one current + one pending WAV request;
- bounded capture arbiter: one current + one pending manual capture request;
- deterministic decision contract: `GRANT / WAIT / REJECT / PREEMPT`;
- equal priority never preempts;
- preemption requires strictly higher priority plus interruptible known owner;
- unknown legacy/external manager activity is never preempted;
- capture/playback hardware submission still goes only through `audio_manager` public APIs and its single operation state.

## 16-E — production client integration

### Xiaozhi capture migration

Production `voice_assistant_uplink.c` no longer semantically owns legacy capture directly. A source-local Phase-16 compatibility bridge redirects its start/stop calls to:

```text
PTT authorized
-> Phase-16 Xiaozhi CAPTURE request
   client=XIAOZHI
   priority=70
   busy_policy=REJECT
   interruptible=true
-> capture arbiter
-> audio_manager_start_recording()
-> manager RECORDING evidence
-> bridge returns success to existing uplink flow
```

PTT capture intentionally uses `REJECT` rather than waiting behind another capture client. If microphone ownership is unavailable, the voice turn does not silently begin late after the user has already spoken.

Release/cancel path:

```text
voice uplink stop
-> capture arbiter cancel(request_id)
-> cooperative audio_manager_stop_recording()
-> manager-owned cleanup / PROCESSING / IDLE
```

The existing `audio_manager_stream_arm/disarm()` and Xiaozhi listening/channel semantics remain owned by the Phase-14 voice path.

### Xiaozhi playback migration

Production `voice_assistant_downlink.c` now reaches WAV playback through a source-local bridge:

```text
response WAV ready
-> Phase-16 Xiaozhi PLAYBACK request
   client=XIAOZHI
   priority=70
   busy_policy=QUEUE
   interruptible=true
-> playback arbiter
-> audio_manager_play_wav()
-> real manager PLAYBACK evidence
-> bridge returns success
-> existing downlink completion wait continues
```

The bridge waits for real arbiter/manager start evidence before returning success so the Phase-14 `downlink_wait_playback_complete()` semantics are preserved. Command acceptance alone is not treated as playback start.

Xiaozhi response playback remains interruptible by a strictly higher-priority known client such as a critical alarm.

### Notification and alarm helpers

Added minimal public playback helpers; these are policy integration surfaces, not new product features:

```c
audio_manager_play_notification_wav(request_id, path);
audio_manager_play_critical_alarm_wav(request_id, path);
```

Notification uses the default model:

```text
client=NOTIFICATION
priority=50
busy_policy=QUEUE
interruptible=true
```

Critical alarm uses:

```text
client=ALARM
priority=100
busy_policy=PREEMPT_LOWER_PRIORITY
interruptible=false
```

Both only submit copied WAV requests to the playback arbiter. They receive no I2S/file/DMA hardware ownership.

### Expected policy examples

```text
Xiaozhi SPEAKING (70, interruptible)
+ Notification (50, QUEUE)
-> notification waits in bounded pending slot if available

Xiaozhi SPEAKING (70, interruptible)
+ Critical Alarm (100, PREEMPT)
-> cooperative Xiaozhi playback stop
-> alarm promoted after manager returns IDLE

Critical Alarm active (100, non-interruptible)
+ Xiaozhi response (70)
-> Xiaozhi cannot preempt alarm

Xiaozhi LISTENING (70 capture)
+ lower-priority recorder
-> lower-priority request follows its own busy policy; it cannot preempt Xiaozhi
```

## Concurrency review

Phase 16 arbitrates **audio requests only**. It does not stop or globally serialize unrelated work.

The following remain allowed to run while Xiaozhi records/sends/plays:

```text
sensor tasks
Firebase/cloud tasks
Wi-Fi/event tasks
GUI/LVGL task
SD manager/recovery work
performance monitor
Xiaozhi transport tasks
```

Required boundaries remain:

```text
sensor/Firebase/network work
-> may contend for CPU/network/memory
-> must be measured during HIL/integration
-> must not acquire audio hardware ownership

GUI
-> receives copied status/model
-> Xiaozhi/audio callbacks never call LVGL directly

audio
-> all capture/playback operations remain behind audio_manager + Phase-16 arbiters
```

Do not introduce hidden `PTT -> pause Firebase` behavior without target evidence that coexistence is unsafe.

## Static-review corrections during 16-E

1. Fixed the Phase-16 policy header to include the real `audio_manager_arbitration.h` contract instead of a nonexistent request header; this was a real compile blocker found before closure.
2. Xiaozhi capture/playback bridges preserve Phase-14 semantic expectations by waiting for real RECORDING/PLAYBACK evidence before returning start success.
3. Xiaozhi capture request IDs and playback request IDs are generated independently and are non-zero project correlation IDs only.
4. Notification/alarm helpers use existing default policy metadata rather than duplicating priority logic.
5. No direct I2S/DMA/file hardware ownership was added to voice, notification or alarm clients.

## Known limitations after 16-E

- full ESP-IDF build has not been run on this branch;
- HIL has not verified arbitration timing/preemption;
- source-local CMake compatibility redirects remain a maintainability seam and must be included in 16-F review;
- playback/capture arbiter stop/deinit lifecycle APIs are still absent;
- cross-resource global fairness is not guaranteed;
- recorded-audio playback is not migrated to arbitration;
- notification/alarm helpers exist but no new end-user alarm/notification product feature is added;
- actual Xiaozhi codec/downlink acceptance remains a Phase-14 HIL dependency.

## Next checkpoint — FINAL Phase-16 prompt after user says `tiếp tục`

**16-F — Final Review / Closure / Deferred HIL**

Planned scope:

1. review Phase-16 diff and production composition end-to-end;
2. inspect lifecycle/task/resource ownership and CMake bridge risks;
3. verify AI_Stored_Data/project state/backlog consistency;
4. create Phase-16 HIL plan and recommended dedicated test branch;
5. classify remaining issues as blockers vs accepted technical debt;
6. if no software blocker remains, close Phase 16 as `Software Complete / Build + HIL Pending`;
7. explicitly state that Phase 16 ends the planned major feature-coding stage and do not start Phase 17 automatically.
