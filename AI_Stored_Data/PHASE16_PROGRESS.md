# Phase 16 — Audio Arbitration & Multi-Client Audio Policy

Updated: 2026-08-25
Branch: `phase/16-audio-arbitration`
Base branch: `phase/15-voice-assistant-ui`
Current checkpoint: **16-A — Audio Client / Request Model**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 16 follows the same checkpoint workflow as Phases 13-15. Implement one checkpoint, stop, explain the result, and continue only after Hải explicitly says `tiếp tục`.

Planned checkpoints:

1. 16-A — audio client/request model. ✅
2. 16-B — playback arbitration runtime. NEXT
3. 16-C — capture arbitration runtime.
4. 16-D — priority/preemption/queue policy hardening.
5. 16-E — Xiaozhi + notification/alarm integration/stubs and concurrency review.
6. 16-F — final review/docs/deferred HIL.

At 16-F explicitly notify Hải that it is the final Phase-16 prompt before software closure.

## Why Phase 16 exists

Through Phase 15, `audio_manager` is already the sole I2S/DMA owner. That prevents direct hardware ownership conflicts, but it does not identify competing legitimate clients or decide which request should win.

Example unresolved cases before Phase 16:

```text
Xiaozhi SPEAKING + notification playback
Xiaozhi SPEAKING + critical alarm
Xiaozhi LISTENING + another recorder request
```

FreeRTOS tasks may continue running concurrently. Phase 16 is about **audio resource arbitration**, not globally pausing unrelated tasks.

## 16-A — implemented

Added:

```text
components/audio/audio_manager/include/audio_manager_arbitration.h
components/audio/audio_manager/audio_manager_arbitration.c
```

and compiled the new model through the existing `audio_manager` component.

### Logical clients

```text
SYSTEM
XIAOZHI
NOTIFICATION
ALARM
RECORDER
UI
TEST
```

These are metadata identities only. No client receives an I2S/DMA/file/raw-buffer handle.

### Resource classes

```text
CAPTURE
PLAYBACK
```

### Busy-policy intent

```text
REJECT
QUEUE
PREEMPT_LOWER_PRIORITY
```

16-A defines the intent only. Runtime queue/preemption semantics are implemented in later checkpoints.

### Recommended priority defaults

```text
BACKGROUND      20
UI              30
NOTIFICATION    50
XIAOZHI         70
SYSTEM          90
CRITICAL_ALARM 100
```

Higher numeric value means higher arbitration priority.

Current defaults:

```text
ALARM        priority=100 policy=PREEMPT_LOWER_PRIORITY interruptible=false
SYSTEM       priority=90  policy=REJECT                   interruptible=false
XIAOZHI      priority=70  policy=REJECT                   interruptible=true
NOTIFICATION priority=50  policy=QUEUE                    interruptible=true
UI           priority=30  policy=QUEUE                    interruptible=true
RECORDER     priority=20  policy=REJECT                   interruptible=true
TEST         priority=20  policy=REJECT                   interruptible=true
```

These defaults are policy starting points, not target-accepted behavior. 16-D may refine them after the runtime arbiter exists.

### Request contract

`audio_manager_request_t` contains:

```text
request_id       non-zero correlation identity
client           logical requester
resource         CAPTURE / PLAYBACK
priority         uint8_t, higher wins
busy_policy      REJECT / QUEUE / PREEMPT_LOWER_PRIORITY
interruptible    whether an already-granted operation may be cooperatively preempted
```

Public helpers:

```c
audio_manager_request_make_default(...);
audio_manager_request_validate(...);
audio_manager_client_to_string(...);
audio_manager_resource_to_string(...);
audio_manager_busy_policy_to_string(...);
```

`request_id` is not an ownership token and cannot be used to touch I2S directly.

## Important compatibility rule

16-A intentionally does **not** change existing production APIs or runtime behavior:

```text
audio_manager_start_recording()
audio_manager_stop_recording()
audio_manager_play_recorded()
audio_manager_play_wav()
audio_manager_stop_playback()
```

They retain their Phase-15 semantics until later checkpoints explicitly migrate selected paths behind arbitration.

This prevents a metadata-model checkpoint from silently changing the already-deferred Phase-14/15 voice path.

## Static review notes

1. No new task, queue, mutex, allocation, I2S handle or hardware ownership is introduced in 16-A.
2. Arbitration model is independent from Xiaozhi/application components.
3. Client identity and priority are project-owned metadata only.
4. Runtime queue/preemption behavior is intentionally not claimed yet.
5. Existing sole-I2S-owner rule remains unchanged.
6. No ESP-IDF build or HIL PASS is claimed.

## Next checkpoint — only after user says `tiếp tục`

**16-B — Playback Arbitration Runtime**

Planned scope:

1. inspect the existing `play_recorded` / `play_wav` command acceptance and cancellation path;
2. add one bounded playback ownership/arbitration state inside `audio_manager` without exposing I2S;
3. support request/client identity for playback requests;
4. start conservatively with deterministic busy handling and bounded pending requests;
5. preserve legacy playback APIs through compatibility wrappers so Phase-14/15 behavior is not broken;
6. do not implement capture arbitration until 16-C.
