# Phase 13 HIL Test Plan

Updated: 2026-08-25
Branch: `phase/13-voice-assistant`
Status: **HIL PASS — TARGET ACCEPTED 2026-08-25**

## Rule

These cases were executed on the dedicated `test/phase13-voice-assistant-hil`
branch. PASS below is backed by ESP32-S3 serial/runtime evidence.

## Execution result — 2026-08-25

| Case | Result | Target evidence |
| --- | --- | --- |
| 1. Normal lifecycle | PASS | 20/20 cycles completed twice; every generation reached real WebSocket READY and returned to IDLE without panic/assert/WDT. |
| 2. Connect failure | PASS | With AP unavailable, CONNECTING reached ERROR in 7140 ms with `session_active=false`. |
| 3. Explicit recovery | PASS | Transport-loss and failed-connect paths executed `ERROR -> RECOVERING -> IDLE`. |
| 4. Transport loss after READY | PASS | Real AP removal produced coordinator `ONLINE -> OFFLINE` and voice `READY -> ERROR`. |
| 5. Intentional-stop late callback | PASS | Repeated intentional disconnects returned to IDLE without a later `IDLE -> ERROR` regression. |
| 6. Audio status burst | PASS | 32 rapid copied-status posts succeeded and lifecycle commands kept making progress. |
| 7. Generation protection | PASS | Generations advanced monotonically and no prior generation mutated a newer session. |

The final regression boot used 3 automatic cycles after the HIL harness was tightened to require coordinator state other than `ONLINE` before accepting manual AP loss. The board returned to `ONLINE` after AP restoration without reset.

## Test matrix

### 1. Normal production session lifecycle

Procedure:

1. Boot Gateway to stable network-online state.
2. Start voice assistant composition.
3. Begin one session.
4. Wait for READY.
5. End session.
6. Repeat at least 20 times.

Expected semantic logs:

```text
VOICE_ASSISTANT: begin session queued generation=N
VOICE_ASSISTANT: state IDLE -> CONNECTING ...
XZ_SESSION: ... -> READY generation=N active=yes ...
XZ_SESSION: WebSocket production session READY generation=N
VOICE_ASSISTANT: state CONNECTING -> READY ...
VOICE_ASSISTANT: end session queued generation=N
XZ_SESSION: DISCONNECTED observed during intentional stop   # allowed if emitted
XZ_SESSION: ... -> STOPPED generation=N active=no ...
VOICE_ASSISTANT: state READY -> IDLE ...
```

Acceptance:

- no panic/assert/WDT;
- no IDLE -> ERROR regression after an intentional stop;
- every accepted generation reaches exactly one stable terminal state;
- repeated lifecycle does not accumulate tasks/handles visibly.

### 2. Connect failure / bounded escape

Make service/network unavailable before `begin_session()`.

Expected:

```text
VOICE_ASSISTANT: state IDLE -> CONNECTING
XZ_SESSION: ... error ...
VOICE_ASSISTANT: state CONNECTING -> ERROR
```

The foundation connect wait is bounded to 15 seconds. A failed connection must
not be reported as `session_active=true` if READY was never reached.

### 3. Explicit recovery

After one ERROR:

```text
voice_assistant_recover()
```

Expected:

```text
VOICE_ASSISTANT: recovery queued generation=N
VOICE_ASSISTANT: state ERROR -> RECOVERING
... bounded foundation cleanup if still active ...
VOICE_ASSISTANT: state RECOVERING -> IDLE
```

Acceptance:

- no automatic reconnect loop;
- one recovery command causes at most one cleanup transaction;
- cleanup failure returns to ERROR with the new error preserved.

### 4. Transport loss after READY

After READY, remove AP/Internet/service connectivity.

Expected:

```text
XZ_SESSION: ... -> ERROR generation=N active=yes ...
VOICE_ASSISTANT: ... -> ERROR generation=N ...
```

Then run explicit recovery and confirm IDLE.

### 5. Intentional-stop late callback ordering

Run repeated end-session while observing disconnect/goodbye ordering.

Allowed log:

```text
XZ_SESSION: DISCONNECTED observed during intentional stop
```

Not allowed after stop completes:

```text
VOICE_ASSISTANT: state IDLE -> ERROR
```

A same-generation late ERROR queued behind the completed END command must be
ignored while the orchestrator is IDLE/RECOVERING.

### 6. Audio status burst / queue-pressure behavior

Exercise audio-manager state callbacks rapidly while no voice lifecycle command
is running, then begin/end a session.

Expected property:

- audio status is latest-value coalesced;
- rapid audio callbacks do not enqueue an unbounded number of voice commands;
- BEGIN/END/foundation status events still make forward progress;
- latest copied audio state eventually appears in `voice_assistant_status_t`.

### 7. Generation stale-event protection

Run session N, stop it, then start session N+1. Observe any late transport event
from the old generation.

Expected marker when applicable:

```text
VOICE_ASSISTANT: Dropped stale Xiaozhi status generation=N ...
```

No old event may mutate generation N+1.

## Known software boundary before HIL

`voice_assistant_begin_session()` currently executes the foundation's bounded
connection startup inside the single orchestration task. While that call is in
progress, duplicate begin/end/recover requests are rejected by the public
pending-command gate. Cancellation is therefore bounded by the existing
15-second connect timeout rather than preempting the underlying startup call.
Do not treat this as an infinite wait.

Phase-12 validation and Phase-13 production session are separate lifecycle
surfaces. Final production composition must not request the Phase-12 validator
while a production voice session is active. This is also reviewed in Phase 13-E.

## Hardware acceptance boundary

The above lifecycle/recovery tests passed on ESP32-S3 with normal Gateway services active on 2026-08-25. Real microphone->Xiaozhi->speaker conversation belongs to Phase 14 and remains independently pending.
