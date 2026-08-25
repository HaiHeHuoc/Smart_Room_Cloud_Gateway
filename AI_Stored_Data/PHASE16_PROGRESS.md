# Phase 16 — Audio Arbitration & Multi-Client Audio Policy

Updated: 2026-08-25
Branch: `phase/16-audio-arbitration`
Base branch: `phase/15-voice-assistant-ui`
Current checkpoint: **16-B — Playback Arbitration Runtime**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 16 follows the same checkpoint workflow as Phases 13-15. Implement one checkpoint, stop, explain the result, and continue only after Hải explicitly says `tiếp tục`.

Planned checkpoints:

1. 16-A — audio client/request model. ✅
2. 16-B — playback arbitration runtime. ✅
3. 16-C — capture arbitration runtime. NEXT
4. 16-D — priority/preemption/queue policy hardening.
5. 16-E — Xiaozhi + notification/alarm integration/stubs and concurrency review.
6. 16-F — final review/docs/deferred HIL.

At 16-F explicitly notify Hải that it is the final Phase-16 prompt before software closure.

## Why Phase 16 exists

`audio_manager` remains the sole I2S/DMA owner, but Phase 16 adds policy for multiple legitimate clients requesting CAPTURE/PLAYBACK without giving any client hardware ownership.

## 16-A summary

Added project-owned request metadata:

```text
client: SYSTEM / XIAOZHI / NOTIFICATION / ALARM / RECORDER / UI / TEST
resource: CAPTURE / PLAYBACK
priority: uint8_t, higher wins
busy_policy: REJECT / QUEUE / PREEMPT_LOWER_PRIORITY
interruptible: cooperative-preemption eligibility
request_id: non-zero correlation identity
```

## 16-B — implemented playback arbitration runtime

Added:

```text
components/audio/audio_manager/include/audio_manager_playback_arbiter.h
components/audio/audio_manager/audio_manager_playback_arbiter.c
```

and compiled the runtime into the existing `audio_manager` component.

### Ownership model

The arbiter does **not** own I2S, DMA, SD files or playback buffers. It only coordinates through existing public APIs:

```text
client request
-> playback arbiter
-> audio_manager_play_wav()
-> audio_manager task
-> existing source/I2S TX path
```

Cancellation/preemption uses:

```text
playback arbiter
-> audio_manager_stop_playback()
-> cooperative manager-owned cleanup
```

### Runtime state

```text
IDLE
STARTING
ACTIVE
PREEMPTING
ERROR
```

Important semantic boundary:

```text
audio_manager_play_wav() returns ESP_OK
!= proof that I2S playback already started
```

Therefore the arbiter remains `STARTING` after command acceptance and only enters `ACTIVE` after a copied `audio_manager_status_t` reports `AUDIO_MANAGER_STATE_PLAYBACK`. Completion is recognized only after a real `PLAYBACK -> IDLE` observation.

### Bounded request storage

The runtime stores:

```text
1 current playback request
1 pending playback request
```

WAV paths are copied into bounded `AUDIO_MANAGER_WAV_PATH_MAX_BYTES` storage before submit returns.

There is no unbounded queue/allocation.

### Busy policies implemented for playback

`REJECT`:

```text
current exists -> reject new request
```

`QUEUE`:

```text
current exists + pending slot empty
-> accept into one pending slot
```

`PREEMPT_LOWER_PRIORITY`:

```text
new.priority > current.priority
AND current.interruptible == true
```

If current has not yet been submitted, the higher-priority request replaces it without touching hardware.

If current is already STARTING/ACTIVE, higher-priority request occupies the pending slot and the arbiter requests cooperative stop once playback is observed. After manager returns IDLE, the pending request is promoted.

### Legacy/external playback safety

Phase-14/15 existing callers have not yet been migrated to the arbiter. If playback is already active outside this arbiter, the runtime treats it as external/unknown ownership and waits for `audio_manager` to return IDLE. It does not preempt unknown legacy playback because no client/interruptibility metadata exists.

Actual Xiaozhi/notification/alarm migration is intentionally deferred until 16-E after playback + capture arbitration and policy hardening are complete.

### Diagnostics/status

Copied arbiter status exposes:

```text
current request metadata
pending request metadata
accepted_count
rejected_count
queued_count
preemption_count
completed_count
failed_count
last_error
```

No raw hardware handles are exposed.

### Production composition

After `audio_manager_start()` succeeds, production composition now initializes/starts the playback arbiter before starting the voice stack:

```text
audio_manager READY
-> playback arbiter READY
-> voice/PTT/UI/uplink/downlink stack
```

The arbiter is independent from Xiaozhi validation ownership because it owns no Xiaozhi resource.

## Static review corrections made during 16-B

1. Fixed false-completion risk caused by treating command acceptance as playback start.
2. Added explicit `STARTING -> ACTIVE` transition only on real manager PLAYBACK evidence.
3. Cancellation before command submission now removes the request cleanly.
4. Preemption before command submission prefers the higher-priority request without issuing unnecessary stop operations.
5. Playback failure vs normal completion is derived from manager `last_error` at returned-IDLE observation.
6. Unknown legacy playback is never preempted.

## Known limitation after 16-B

- playback arbiter currently has init/start but no dedicated stop/deinit lifecycle API;
- existing production Xiaozhi playback still calls legacy audio-manager playback until 16-E migration;
- recorded-audio playback arbitration is not promoted yet; 16-B public request path is WAV-oriented;
- no build/HIL PASS is claimed.

These are follow-up items, not hidden acceptance claims.

## Next checkpoint — only after user says `tiếp tục`

**16-C — Capture Arbitration Runtime**

Planned scope:

1. arbitrate microphone/manual recording ownership with the same client/request model;
2. keep `audio_manager` as sole I2S RX owner;
3. support one current + bounded pending capture request;
4. provide cooperative cancellation/preemption through existing manager recording controls;
5. preserve legacy capture APIs until client migration in later checkpoint;
6. review capture vs playback interaction without inventing a second audio hardware owner.
