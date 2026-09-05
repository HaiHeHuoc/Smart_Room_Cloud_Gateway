# Voice Assistant

## Purpose

`voice_assistant` is the project-owned conversation orchestrator. It sits between
`audio_manager`, `xiaozhi_foundation`, and future GUI/PTT composition without
owning those components' hardware or framework objects.

Ownership remains:

- `audio_manager`: sole microphone/speaker/I2S/DMA/PCM owner;
- `xiaozhi_foundation`: sole direct `esp_xiaozhi`/MCP/service/session boundary;
- `voice_assistant`: conversation generation, state, command ordering and recovery policy;
- `app_gui` / `ui_manager_lvgl`: sole GUI/LVGL ownership.

Phase 13 status: **Software Complete / HIL Passed**. The later Phase-16.1
streaming path has separate target acceptance requirements.

## State machine

```text
UNINITIALIZED -> INITIALIZED -> IDLE
                              |
                              | begin_session()
                              v
                          CONNECTING
                              |
                              | real Xiaozhi CONNECTED
                              v
                            READY

READY -- unexpected WebSocket loss --> CONNECTING
                                      |
                                      | upstream WebSocket reconnect
                                      v
                                    READY

initial or unrecoverable session failure -> ERROR
                              |
                              | recover()
                              v
                         RECOVERING
                              |
                              | bounded cleanup, no reconnect loop
                              v
                            IDLE

active session -- end_session() --> IDLE
```

`LISTENING`, `THINKING`, and `SPEAKING` are reserved for the explicit
voice-owned PTT/audio transaction introduced in Sprint 14. Generic
`audio_manager` recording or playback does not drive these states.

## Xiaozhi production session

The production session is separate from Phase-12 P2-E/P2-F validation. Session
start requires real WebSocket CONNECTED evidence and remains alive until stop or
failure.

Public foundation surface:

```c
xiaozhi_foundation_session_register_status_callback(...);
xiaozhi_foundation_session_start(generation);
xiaozhi_foundation_session_stop();
xiaozhi_foundation_session_get_status(...);
```

Cleanup order remains:

```text
chat stop
-> event handler unregister
-> chat deinit
-> MCP destroy
-> EventGroup delete
```

The session layer distinguishes intentional stop from unexpected transport loss.
DISCONNECTED generated during explicit stop is teardown evidence instead of a
false runtime failure. During an active production session, an unexpected
DISCONNECTED clears the stale audio-channel state and leaves the session in
`CONNECTING` so the upstream WebSocket client's bounded reconnect can return it
to `READY`; the project does not create a second reconnect loop. A
`SERVER_GOODBYE` after a completed audio turn closes only that audio channel and
does not poison the still-connected WebSocket session. If transport loss
interrupts an in-flight response, the copied foundation error bypasses the
READY-only downlink gate and aborts that incomplete response before the next
turn is allowed.

The pinned provider sends WebSocket text and binary frames with
`portMAX_DELAY`. Project link wrappers change only those public API requests to
an 8-second timeout; provider source and private transport handles remain
unchanged, while the existing session-close and reconnect policies remain
independent.

After network `ONLINE` and audio startup, the Phase-14 composition queues one
`voice_assistant_begin_session()` call. This establishes the long-lived service
connection without opening a microphone/audio channel; GPIO38 remains the sole
user authorization for a conversation turn.

## Callback and stale-event policy

Xiaozhi callbacks never mutate voice state directly:

```text
Xiaozhi event loop
-> copied foundation status
-> bounded voice queue / one latest pending snapshot
-> voice task
-> session-generation check
-> state transition
```

Late events from an older generation are dropped. A same-generation late ERROR
after an intentional stop has already returned to IDLE is ignored rather than
regressing IDLE back to ERROR. Foundation and audio producers retain one latest
copied snapshot if the command queue is temporarily full; after consuming any
command, the voice task drains that snapshot. A queue-pressure transient
therefore cannot leave the UI with an obsolete terminal state.

## Explicit recovery policy

`voice_assistant_recover()` is accepted only from `ERROR`:

```text
ERROR -> RECOVERING
      -> query foundation session status
      -> stop/cleanup once if still active
      -> IDLE on success
      -> ERROR on cleanup failure
```

There is no project-owned automatic reconnect loop. The upstream WebSocket
client reconnects the retained active session after transport loss. When an
initial session cannot be recovered, a later explicit `begin_session()` starts
a fresh generation.

PTT additionally retains a still-held press across one bounded `ERROR` recovery:
when cleanup returns the voice state to `IDLE`, it starts the fresh session and
waits for real `READY`. Releasing before `READY` always cancels and never
authorizes capture.

A physical release remains FIFO-deliverable behind a pending press. Therefore a
short GPIO38 tap that ends before the PTT policy task processes its press is
reported as released and never authorizes microphone capture.

The GPIO adapter separately tracks a debounced electrical level and its PTT
policy delivery. If a stable edge is temporarily rejected while the PTT queue
drains, it retries at a bounded interval while the physical level remains
unchanged; it does not invent another button edge.

For a non-empty turn, uplink reserves a downlink-owned response wait before it
sends stop-listening. The wait serializes PTT until TTS starts, an error arrives,
or its finite timeout aborts the channel; it also covers finalization and
playback, so an ignored busy press is never reported as capture-authorized.

## Audio contract

`voice_assistant_audio_adapter_post()` translates one existing copied
`audio_manager_status_t` into `voice_assistant_audio_status_t`. No I2S handle,
DMA descriptor, PCM pointer or private recording storage crosses the boundary.

Audio notifications use latest-value coalescing. While one audio marker is
pending in the command queue, newer audio snapshots overwrite the private copied
pending value instead of consuming additional queue slots. If the queue is full
when the first marker is posted, that pending value is retained and drained by
the next command instead of being dropped.

Phase 16.1 routes decoded Xiaozhi response PCM16 through the public streaming
and playback-arbiter contracts. The downlink worker is the sole decoder and
producer; `audio_manager` remains the sole I2S/DMA owner. Its copied Opus queue
is bounded, the manager-owned PCM ingress ring is bounded in PSRAM, and a
damaged packet taints only its owning response generation.

Response control uses three separate bounds: a 15-second server-inactivity
watchdog while waiting/collecting, a 90-second post-`TTS_STOP` PCM drain
watchdog, and a ten-minute absolute response ceiling. Finalizing never falls
through to the short inactivity watchdog.

## GUI contract

`voice_assistant_status_t` is a production UI-safe copied model containing:

- conversation state;
- session generation;
- session-active flag;
- latest voice error;
- copied audio lifecycle state;
- capture/playback-active flags;
- latest audio error.

`voice_assistant` does not call LVGL. Transcript/emotion presentation and the
final GUI voice queue/rendering belong to Sprint 15.

## Task and queue

- one long-lived `voice_assistant` task;
- stack: 8192 bytes in Internal RAM;
- priority: 4;
- command queue length: 8;
- bounded mutex wait: 100 ms;
- task-start readiness wait: 2 seconds;
- one public lifecycle command pending at a time;
- foundation and audio updates coalesced latest-value style, including queue-full deferral;
- status callbacks run only after releasing the voice status mutex.

## Public API

```c
esp_err_t voice_assistant_init(void);
esp_err_t voice_assistant_start(void);
esp_err_t voice_assistant_begin_session(void);
esp_err_t voice_assistant_end_session(void);
esp_err_t voice_assistant_recover(void);
esp_err_t voice_assistant_notify_audio_status(
    const voice_assistant_audio_status_t *status);
esp_err_t voice_assistant_register_status_callback(...);
esp_err_t voice_assistant_get_status(...);
```

Composition-only audio adapter:

```c
esp_err_t voice_assistant_audio_adapter_post(
    const audio_manager_status_t *status);
```

## Production vs Phase-12 validation

The repository retains the Phase-12 validation composition and assets for HIL.
The Phase-13 production branch explicitly defaults:

```text
CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=n
```

so normal Phase-13 firmware does not automatically run the temporary validator.
Dedicated HIL/test branches may opt in explicitly.

Phase 13 deliberately did not auto-call `voice_assistant_begin_session()` at
boot. The current Phase-14 composition queues the connection after `ONLINE`,
but a conversation remains user-authorized: Sprint 14 owns the GPIO38 PTT
trigger, mic uplink and response-audio lifecycle.

## Security boundary

Phase 13 exposes no voice API for reboot, OTA, NVS erase/write, Wi-Fi
reconfiguration, provisioning lifecycle, arbitrary GPIO/driver control or
shell/system commands.

Voice recovery may clean only the voice-owned Xiaozhi session. It may not take
over Wi-Fi, provisioning, cloud, reset, storage, audio hardware or GUI lifecycle.

## Verification boundary

Static/source review and the Phase-14 golden-path target HIL are complete. The
corrected ESP32-S3 image built and flashed successfully, and three GPIO38 PTT
turns completed Opus uplink, response decode, SD-backed WAV playback and
operator-confirmed audible output before returning to `IDLE`. Fault-injection
cases (stalled response, network loss, SD unavailable and queue pressure) are
explicitly deferred; see `AI_Stored_Data/PHASE14_HIL_TEST_PLAN.md`.

The current targeted transport regression trace has verified boot connection,
unexpected-disconnect reconnect, a response-pending GPIO38 press rejection,
and a GPIO38 turn through playback completion. Audible speaker acceptance and
the remaining Phase-14 fault matrix on that exact image remain separate
hardware checks.
