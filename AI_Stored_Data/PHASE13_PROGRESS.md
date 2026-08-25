# Phase 13 Voice Assistant Progress

Updated: 2026-08-25
Branch: `phase/13-voice-assistant`
Current checkpoint: **13-D — Failure / Recovery + Software Validation**
Status: **IMPLEMENTED / STATIC REVIEW COMPLETE / BUILD + HIL NOT CLAIMED**

## Collaboration rule

Phase 13 is implemented in reviewable parts. After each part, stop coding and
wait for Hải to review. Continue only after the user explicitly says `tiếp tục`.

## 13-A — completed

Implemented the `voice_assistant` component foundation:

- one long-lived orchestration task;
- bounded queue/mutex/start wait;
- non-zero session generation;
- stale command rejection;
- conversation states from UNINITIALIZED through ERROR/RECOVERING.

## 13-B — completed

Added the production Xiaozhi WebSocket session boundary in
`xiaozhi_foundation`:

```text
begin session
-> CONNECTING
-> public Xiaozhi get_info / MCP / chat init
-> real CONNECTED event
-> READY
-> explicit stop/failure
-> ownership-ordered cleanup
```

The production session is distinct from the Phase-12 validation worker and no
Xiaozhi/MCP handle or sensitive value crosses the public boundary.

## 13-C — completed

Added the project-owned audio/UI-safe status contract:

- `voice_assistant_audio_status_t`;
- copied audio state inside `voice_assistant_status_t`;
- `voice_assistant_notify_audio_status()`;
- `voice_assistant_audio_adapter_post()` for application fan-out from the
  existing `audio_manager` callback.

No I2S/DMA/PCM ownership moved out of `audio_manager`. Generic audio activity
does not automatically become LISTENING/THINKING/SPEAKING.

## 13-D — implemented

### Intentional stop vs unexpected disconnect

`xiaozhi_session.c` now tracks explicit stop intent. DISCONNECTED or
SERVER_GOODBYE observed while `session_stop()` is cleaning up are treated as
expected teardown evidence instead of publishing a false transport ERROR.

Unexpected disconnect still produces ERROR.

### Correct active-state semantics during CONNECTING

Before 13-D, DISCONNECTED/SERVER_GOODBYE hard-coded `active=true`. A failed
connection could therefore publish an ERROR claiming an active session even if
READY was never reached.

13-D snapshots the pre-event `s_status.active` value:

```text
CONNECTING failure -> ERROR active=false
READY transport loss -> ERROR active=true
```

### Late callback hardening

`voice_assistant` now accepts READY only while CONNECTING/READY. It ignores a
same-generation late ERROR when the orchestrator has already completed an
intentional stop and returned to IDLE, or while bounded recovery is already in
progress.

Generation mismatch is still dropped before any state transition.

### Explicit bounded recovery

Added:

```c
esp_err_t voice_assistant_recover(void);
```

Accepted only from ERROR. Flow:

```text
ERROR
-> RECOVERING
-> query foundation session status
-> stop/cleanup once if still active
-> IDLE on success
-> ERROR on cleanup failure
```

There is no automatic reconnect loop. A fresh session requires a later explicit
`begin_session()`.

### Audio queue-pressure policy

Audio status now uses latest-value coalescing instead of one queue item per
callback:

```text
audio callback A -> enqueue one AUDIO marker
audio callback B/C/D before marker consumed -> overwrite copied pending value
voice task consumes marker -> applies latest D
```

This prevents a burst of audio-status callbacks from filling the bounded command
queue needed for lifecycle and Xiaozhi transport events.

### Duplicate / command ordering policy

The existing `s_command_pending` gate continues to allow only one public
begin/end/recover command in flight. Duplicate lifecycle requests are rejected
with `ESP_ERR_INVALID_STATE` instead of accumulating.

The synchronous foundation connection startup is bounded to 15 seconds. While
it is executing inside the voice task, public duplicate/end/recover requests are
rejected rather than pretending to preempt an upstream operation that has no
project cancel API.

### Deferred HIL plan

Created:

`AI_Stored_Data/PHASE13_HIL_TEST_PLAN.md`

It records expected logs and acceptance for:

- normal repeated session lifecycle;
- connect failure;
- explicit recovery;
- transport loss after READY;
- intentional-stop late callbacks;
- audio callback bursts / queue pressure;
- stale generation events.

Hardware is unavailable, so all target evidence remains **DEFERRED HIL**.

## Remaining closure findings for 13-E

1. `main.c` still contains the temporary Phase-12 Xiaozhi validation
   composition. Phase 13-E must decide/implement the production composition
   boundary without accidentally running validation and production session
   lifecycles together.
2. Phase-12 validation and Phase-13 production session remain separate public
   lifecycle surfaces. The current production rule is "do not run them
   concurrently"; final composition review must ensure this is enforced by the
   application path used in production.
3. No public live PCM stream exists in `audio_manager`; do not bypass private
   buffers. Real microphone/audio-channel/response-audio work belongs to the
   later PTT integration phase.
4. No ESP-IDF build or target HIL is claimed in this session.

## Next checkpoint — only after user says `tiếp tục`

**13-E — FINAL PHASE-13 PROMPT: Final Review + Composition + Docs + Deferred HIL**

This is the final planned coding/review checkpoint of Phase 13.

Planned scope:

1. full Phase-13 diff/ownership/concurrency/lifetime review;
2. integrate `voice_assistant` into production composition where safe;
3. remove/disable temporary Phase-12 runtime orchestration that conflicts with
   the production session path, without deleting deferred validation assets;
4. wire copied status fan-out needed by composition while keeping LVGL/I2S
   ownership unchanged;
5. review compile surface and documentation drift;
6. update roadmap/project state/AI_Stored_Data;
7. leave hardware-dependent acceptance explicitly deferred;
8. if no software blocker remains, declare **Phase 13 Software Complete / HIL
   Pending** and notify Hải that this is the prompt that completes Phase 13.
