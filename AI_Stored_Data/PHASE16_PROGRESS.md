# Phase 16 — Audio Arbitration & Multi-Client Audio Policy

Updated: 2026-08-25
Branch: `phase/16-audio-arbitration`
Base branch: `phase/15-voice-assistant-ui`
Current checkpoint: **16-D — Priority / Preemption / Queue Policy Hardening**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 16 follows the same checkpoint workflow as Phases 13-15. Implement one checkpoint, stop, explain the result, and continue only after Hải explicitly says `tiếp tục`.

Planned checkpoints:

1. 16-A — audio client/request model. ✅
2. 16-B — playback arbitration runtime. ✅
3. 16-C — capture arbitration runtime. ✅
4. 16-D — priority/preemption/queue policy hardening. ✅
5. 16-E — Xiaozhi + notification/alarm integration/stubs and concurrency review. NEXT
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

## 16-C summary — capture runtime

Capture arbitration uses one current + one pending request. It controls recording only through `audio_manager_start_recording()` / `audio_manager_stop_recording()` and copied manager status. RECORDING is real ACTIVE evidence; PROCESSING is FINISHING and is allowed to complete before pending promotion.

## 16-D — implemented deterministic policy hardening

Added a shared project-owned decision contract:

```text
audio_manager_arbitration_policy_decide(incoming, owner)
-> GRANT / WAIT / REJECT / PREEMPT
```

Rules are deterministic across CAPTURE and PLAYBACK request metadata:

```text
no known owner
-> GRANT

busy_policy = REJECT
-> REJECT

busy_policy = QUEUE
-> WAIT

busy_policy = PREEMPT_LOWER_PRIORITY
+ current interruptible
+ incoming priority strictly greater than current
-> PREEMPT

same priority
-> never preempt

lower priority
-> never preempt
```

This deliberately avoids equal-priority ping-pong and makes preemption a strictly-higher-priority operation.

### Cross-resource rule

Capture and playback arbiters remain structurally separate, but the underlying `audio_manager` has one operation state. Therefore a request may only be submitted to hardware when copied manager state is IDLE. A CAPTURE request and PLAYBACK request can both exist logically in their bounded arbiter slots, but only one may become the manager operation at a time.

16-D does not introduce a second hardware scheduler or I2S owner. When both arbiters observe IDLE near-simultaneously, the existing `audio_manager` command serialization remains the final hardware gate; the loser sees `ESP_ERR_INVALID_STATE` and waits/retries after manager returns IDLE. This is safe but does not guarantee global fairness between the two independent arbiter tasks.

### Fairness / starvation decision

For the current embedded MVP, no unbounded fairness queue is introduced. Each resource has exactly one current + one pending request. Priority is deterministic inside each resource. Cross-resource fairness is intentionally not claimed because capture and playback have separate bounded arbiters above one manager state machine.

If HIL/integration proves starvation under sustained competing CAPTURE/PLAYBACK load, the next architectural step is a single unified audio scheduler above both resources. Do not add an unbounded queue merely to claim fairness before evidence exists.

### Pending-slot behavior

- one pending slot only;
- if pending is occupied, additional QUEUE requests are rejected;
- a higher-priority request does not silently replace an already accepted pending request;
- cancellation explicitly removes matching pending/current ownership;
- request IDs must remain unique among current/pending slots;
- unknown legacy/external manager activity is never preempted because trusted client/interruptibility metadata does not exist.

### Diagnostics

Both arbiters expose bounded copied status including current/pending request metadata and counters for accepted, rejected, queued, preempted, completed and failed operations. These counters are the intended HIL evidence for policy behavior.

### Known limitations after 16-D

- cross-resource global fairness is not guaranteed; manager serialization is safe but separate arbiter tasks can race for IDLE;
- arbiter stop/deinit lifecycle APIs are still not implemented;
- recorded-audio playback is not promoted into request arbitration;
- production Xiaozhi capture/playback is still legacy until 16-E;
- notification/alarm clients are not yet integrated;
- no build/HIL PASS is claimed.

## Next checkpoint — only after user says `tiếp tục`

**16-E — Xiaozhi + Notification/Alarm Integration and Concurrency Review**

Planned scope:

1. migrate Phase-14 Xiaozhi capture/playback requests to arbitration metadata without bypassing `audio_manager`;
2. add minimal notification/alarm request helpers or stubs sufficient to exercise policy without inventing a new product feature;
3. verify Xiaozhi is interruptible/non-interruptible at the correct points;
4. review simultaneous sensor/Firebase/UI work: unrelated tasks continue; only audio requests are arbitrated;
5. preserve bounded callbacks/queues and existing PTT semantics;
6. prepare final integration surface for 16-F closure/HIL.
