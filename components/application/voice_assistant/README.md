# Voice Assistant

## Purpose

`voice_assistant` is the application orchestration layer for Phase 13. It is
intentionally separate from `audio_manager`, `xiaozhi_foundation`, and `app_gui`.

Ownership is fixed as follows:

- `audio_manager` remains the sole production I2S/audio-hardware owner;
- `xiaozhi_foundation` remains the Xiaozhi transport/service boundary;
- `app_gui`/the LVGL UI task remain the only GUI owners;
- `voice_assistant` owns conversation state, session generation, and orchestration decisions only.

Phase 13-A implements only the foundation/state-machine layer. It does not
start Xiaozhi, capture microphone audio, play response audio, or call LVGL.

## Current state machine

```text
UNINITIALIZED
    |
    | init()
    v
INITIALIZED
    |
    | start()
    v
   IDLE
    |
    | begin_session()
    v
CONNECTING
    |
    | Phase 13-B transport evidence
    v
  READY
    |
    +--> LISTENING   (reserved for later Phase 13/14 integration)
    +--> THINKING
    +--> SPEAKING
    +--> RECOVERING
    +--> ERROR

Any active session
    |
    | end_session()
    v
   IDLE
```

Phase 13-A deliberately does **not** synthesize `CONNECTING -> READY`. READY
must be driven later by real Xiaozhi transport evidence.

## Task and queue

A single `voice_assistant` task owns state-machine command execution.

- stack: 4096 bytes;
- priority: 4;
- command queue length: 8;
- APIs use bounded mutex waits;
- task start waits at most 2 seconds for the task to reach `IDLE`;
- callbacks are invoked only after the component status lock has been released.

`begin_session()` and `end_session()` enqueue bounded commands rather than
performing transport/audio work in the caller context.

## Session generation

Each accepted `begin_session()` allocates a non-zero monotonically advancing
`session_generation`. Commands carry that generation and the task drops stale
commands that do not match the current generation.

This is the foundation for Phase 13-B callback filtering: a late Xiaozhi event
from an older session must not mutate the next conversation.

## Public API

```c
esp_err_t voice_assistant_init(void);
esp_err_t voice_assistant_start(void);
esp_err_t voice_assistant_begin_session(void);
esp_err_t voice_assistant_end_session(void);
esp_err_t voice_assistant_register_status_callback(
    voice_assistant_status_callback_t callback,
    void *user_context);
esp_err_t voice_assistant_get_status(voice_assistant_status_t *status);
const char *voice_assistant_state_to_string(voice_assistant_state_t state);
```

The status snapshot contains only project-owned scalar state:

- state;
- session generation;
- session-active flag;
- last `esp_err_t`.

No Xiaozhi handle, endpoint, token, audio pointer, LVGL object, or framework-owned
pointer crosses this boundary.

## Intentionally deferred after Phase 13-A

- Xiaozhi production-facing adapter and transport events;
- `CONNECTING -> READY` transition;
- audio capture/playback integration;
- GUI status adapter;
- transport-loss recovery policy;
- stop/deinit lifecycle for the long-lived orchestrator task;
- hardware/runtime acceptance.

These belong to later Phase 13 parts and must not be simulated merely to make
the state machine appear complete.
