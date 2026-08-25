# Voice Assistant

## Purpose

`voice_assistant` is the project-owned conversation orchestrator. It sits between
`audio_manager`, `xiaozhi_foundation`, and future GUI composition without owning
those components' hardware or framework objects.

Ownership remains:

- `audio_manager`: sole microphone/speaker/I2S/DMA/PCM owner;
- `xiaozhi_foundation`: Xiaozhi transport/session boundary;
- `voice_assistant`: conversation generation, state and orchestration policy;
- `app_gui`: sole GUI/LVGL owner.

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

transport/session failure -> ERROR
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

`LISTENING`, `THINKING`, and `SPEAKING` remain reserved for explicit voice-owned
audio flow. Generic `audio_manager` recording/playback does not drive them.

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

The session layer now distinguishes an intentional stop from an unexpected
transport loss. DISCONNECTED/SERVER_GOODBYE generated during explicit stop are
recorded as expected teardown evidence instead of producing a false ERROR. A
disconnect while CONNECTING also preserves `active=false`; only an already-ready
session reports an active transport failure.

## Callback and stale-event policy

Xiaozhi callbacks never mutate voice state directly:

```text
Xiaozhi event loop
-> copied foundation status
-> bounded voice queue
-> voice task
-> session-generation check
-> state transition
```

Late events from an older generation are dropped. A same-generation late ERROR
arriving after an intentional stop has already returned the voice state to IDLE
is ignored instead of regressing IDLE back to ERROR.

## Explicit recovery policy

`voice_assistant_recover()` is accepted only from `ERROR`. It queues one bounded
recovery operation:

```text
ERROR -> RECOVERING
      -> query foundation session status
      -> stop/cleanup only if still active
      -> IDLE on success
      -> ERROR on cleanup failure
```

Recovery does **not** automatically reconnect and never loops indefinitely.
A later application policy may decide when to begin a fresh session.

## Audio contract

`voice_assistant_audio_adapter_post()` translates the existing copied
`audio_manager_status_t` into `voice_assistant_audio_status_t`. No I2S handle,
DMA descriptor, PCM pointer or private recording storage crosses the boundary.

Audio notifications use latest-value coalescing. While one audio marker is
pending in the command queue, newer audio snapshots overwrite the private copied
pending value instead of enqueueing additional markers. This prevents a burst of
audio status callbacks from consuming all control/event queue slots.

The current `audio_manager` still does not expose a live PCM streaming API, so
Phase 13 does not bypass its private recording buffer. Real microphone-to-Xiaozhi
streaming remains a later integration step.

## GUI contract

`voice_assistant_status_t` is the production UI-safe model. It contains only:

- conversation state;
- session generation;
- session-active flag;
- latest voice error;
- copied audio lifecycle state;
- capture/playback active flags;
- latest audio error.

`voice_assistant` does not call LVGL. Application composition can copy this model
into `app_gui` later.

## Task and queue

- one long-lived `voice_assistant` task;
- stack: 4096 bytes;
- priority: 4;
- command queue length: 8;
- bounded mutex wait: 100 ms;
- task-start readiness wait: 2 seconds;
- one public lifecycle command pending at a time;
- audio updates coalesced latest-value style;
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

## Remaining Phase-13 closure items

- production composition in `main.c` still needs to replace the temporary
  Phase-12 validation orchestration where appropriate;
- Phase-12 validation and Phase-13 production session are still separate public
  lifecycle surfaces. Current application policy must not run them concurrently;
  final composition cleanup must ensure production runtime chooses one surface;
- cancellation while the synchronous 15-second CONNECTING start is inside the
  foundation call is not yet a preemptive cancel. Public duplicate/end commands
  are rejected while that command is pending; the bounded connect timeout is the
  current escape path;
- no live PCM/Opus streaming, response playback or production transcript model;
- no target build/HIL acceptance claimed yet.
