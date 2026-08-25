# Phase 16 — Audio Arbitration & Multi-Client Audio Policy

Updated: 2026-08-25
Branch: `phase/16-audio-arbitration`
Base branch: `phase/15-voice-assistant-ui`
Current checkpoint: **16-C — Capture Arbitration Runtime**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 16 follows the same checkpoint workflow as Phases 13-15. Implement one checkpoint, stop, explain the result, and continue only after Hải explicitly says `tiếp tục`.

Planned checkpoints:

1. 16-A — audio client/request model. ✅
2. 16-B — playback arbitration runtime. ✅
3. 16-C — capture arbitration runtime. ✅
4. 16-D — priority/preemption/queue policy hardening. NEXT
5. 16-E — Xiaozhi + notification/alarm integration/stubs and concurrency review.
6. 16-F — final review/docs/deferred HIL.

At 16-F explicitly notify Hải that it is the final Phase-16 prompt before software closure.

## Why Phase 16 exists

`audio_manager` remains the sole I2S/DMA owner. Phase 16 adds bounded policy for multiple legitimate CAPTURE/PLAYBACK clients without exposing hardware ownership.

## 16-A summary

Project-owned request metadata:

```text
request_id
client
resource = CAPTURE / PLAYBACK
priority
busy_policy = REJECT / QUEUE / PREEMPT_LOWER_PRIORITY
interruptible
```

## 16-B summary — playback runtime

Playback arbitration uses one current + one pending WAV request. The arbiter never owns I2S, files or DMA and controls playback only through `audio_manager_play_wav()` / `audio_manager_stop_playback()`. STARTING is not considered ACTIVE until real manager PLAYBACK evidence is observed.

## 16-C — implemented capture arbitration runtime

Added:

```text
components/audio/audio_manager/include/audio_manager_capture_arbiter.h
components/audio/audio_manager/audio_manager_capture_arbiter.c
```

and compiled it into the existing `audio_manager` component.

### Ownership

```text
client request
-> capture arbiter
-> audio_manager_start_recording()
-> audio_manager task
-> sole I2S RX owner
```

Cooperative stop/preemption:

```text
capture arbiter
-> audio_manager_stop_recording()
-> manager-owned RX cleanup / DSP completion
```

The arbiter owns no I2S/DMA/raw microphone buffer.

### Runtime states

```text
IDLE
STARTING
ACTIVE
FINISHING
PREEMPTING
ERROR
```

Important lifecycle evidence:

```text
audio_manager_start_recording() ESP_OK
= command accepted, not recording-start proof

RECORDING observed
= ACTIVE

PROCESSING observed after recording
= FINISHING

IDLE after observed recording
= current capture transaction complete/cancelled/preempted
```

The arbiter does not attempt to stop DSP once the manager has moved to PROCESSING; it waits for the existing manager-owned processing path to return IDLE before promoting the pending request.

### Bounded request storage

```text
1 current capture request
1 pending capture request
```

No dynamic/unbounded request queue is introduced.

### Busy policies

`REJECT`: reject when current arbiter owner exists.

`QUEUE`: accept into the one pending slot if empty.

`PREEMPT_LOWER_PRIORITY`: accepted only if current is interruptible, new priority is higher, and the pending slot is empty.

If preemption occurs before real recording starts, the lower-priority current request is replaced without touching hardware. If RECORDING is already active, cooperative stop is requested through `audio_manager_stop_recording()` and the pending request is promoted only after manager cleanup returns IDLE.

### Legacy/external capture safety

Existing Phase-14 Xiaozhi uplink still calls legacy `audio_manager_start_recording()` / `audio_manager_stop_recording()` until 16-E. A recording/processing transaction not owned by the capture arbiter is treated as external/unknown busy and is never preempted because no trusted metadata exists.

### Static-review fixes in 16-C

1. command acceptance is not treated as proof of active recording;
2. capture completion spans RECORDING -> optional PROCESSING -> IDLE;
3. cancel-before-start removes current request cleanly;
4. preempt-before-start replaces lower-priority request without issuing a hardware stop;
5. preemption during PROCESSING waits for natural manager cleanup;
6. unknown legacy capture is never preempted.

### Production composition

After `audio_manager_start()` succeeds:

```text
audio_manager READY
-> playback arbiter READY
-> capture arbiter READY
-> Phase-15 voice/UI/PTT/uplink/downlink stack
```

### Known limitations after 16-C

- capture/playback arbiters currently have init/start but no dedicated stop/deinit API;
- Phase-14 Xiaozhi capture/playback has not yet migrated to the arbiter APIs;
- capture and playback policies are structurally separate but share the underlying audio_manager single-operation state, so 16-D must harden cross-resource ordering/policy;
- request fairness/starvation policy is not finalized;
- no build/HIL PASS is claimed.

## Next checkpoint — only after user says `tiếp tục`

**16-D — Priority / Preemption / Queue Policy Hardening**

Planned scope:

1. review playback and capture arbiters together against the single `audio_manager` state machine;
2. define deterministic cross-resource behavior when capture and playback requests arrive together;
3. harden equal-priority, pending-slot replacement, cancellation and starvation behavior;
4. add/normalize diagnostics needed for integration/HIL;
5. preserve unknown legacy-client safety;
6. do not migrate Xiaozhi/notification/alarm callers until 16-E.
