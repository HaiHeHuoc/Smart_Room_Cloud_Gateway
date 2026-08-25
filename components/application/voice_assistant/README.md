# Voice Assistant

## Purpose

`voice_assistant` is the application orchestration layer for Phase 13. It is intentionally separate from `audio_manager`, `xiaozhi_foundation`, and `app_gui`.

Ownership is fixed as follows:

- `audio_manager` remains the sole production I2S/audio-hardware owner;
- `xiaozhi_foundation` remains the Xiaozhi transport/service boundary;
- `app_gui`/the LVGL UI task remain the only GUI owners;
- `voice_assistant` owns conversation state, session generation, and orchestration decisions only.

Phase 13-B adds the first production-facing Xiaozhi session integration. It still does **not** capture microphone audio, open an audio channel, play response audio, or call LVGL.

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
    | real Xiaozhi CONNECTED event
    v
  READY
    |
    +--> LISTENING   (later Phase 13/14 integration)
    +--> THINKING
    +--> SPEAKING
    +--> RECOVERING
    +--> ERROR

Active session
    |
    | end_session()
    v
   IDLE
```

`CONNECTING -> READY` is no longer synthesized. `xiaozhi_foundation_session_start()` returns success only after the WebSocket chat reports a real CONNECTED event within its bounded timeout.

## Xiaozhi production session boundary

Phase 12 validation still exists separately. Phase 13-B does not reuse the P2-E/P2-F worker as a production session because those workers deliberately connect, validate, and tear down.

The new long-lived foundation contract is:

```c
esp_err_t xiaozhi_foundation_session_register_status_callback(...);
esp_err_t xiaozhi_foundation_session_start(uint32_t client_generation);
esp_err_t xiaozhi_foundation_session_stop(void);
esp_err_t xiaozhi_foundation_session_get_status(...);
```

The foundation session:

1. retrieves Xiaozhi service information;
2. requires WebSocket capability and explicitly disables MQTT preference;
3. creates the MCP engine;
4. creates/initializes the Xiaozhi chat object;
5. registers the public ESP event handler;
6. starts chat and waits at most 15 seconds for CONNECTED;
7. remains alive in READY until explicit stop or asynchronous transport failure;
8. cleans up in the validated Phase-12 ownership order: chat stop -> event handler unregister -> chat deinit -> MCP destroy -> EventGroup delete.

No Xiaozhi handle, MCP handle, token, endpoint, credential, or framework-owned pointer crosses the public foundation API.

## Callback/event flow

Xiaozhi event-loop callbacks do not mutate `voice_assistant` state directly.

```text
Xiaozhi event loop
      |
      | copied foundation status
      v
voice_assistant queue
      |
      v
voice_assistant task
      |
      +--> generation check
      +--> READY / ERROR state transition
```

The foundation copies the application-provided `client_generation` into every session status. `voice_assistant` drops events whose generation no longer matches the active conversation.

This prevents a late transport callback from an older session from mutating a newer conversation.

## Task and queue

A single `voice_assistant` task owns state-machine command execution.

- stack: 4096 bytes;
- priority: 4;
- command queue length: 8;
- APIs use bounded mutex waits;
- task start waits at most 2 seconds for the task to reach `IDLE`;
- callbacks are invoked only after the component status lock has been released.

`begin_session()` and `end_session()` enqueue bounded commands rather than performing transport work in the caller context. Transport setup/teardown executes in the orchestration task, not in Wi-Fi, Xiaozhi event-loop, or GUI callback context.

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

## Current limitations / deferred work

Phase 13-B intentionally does not implement:

- microphone capture or PCM/Opus TX integration;
- Xiaozhi audio-channel open/close for production conversation;
- response-audio routing to `audio_manager`;
- USER/ASSISTANT transcript promotion into the production voice status contract;
- GUI adapter;
- automatic retry/recovery state machine after transport loss;
- production composition/start trigger in `main.c`;
- hardware/runtime acceptance.

The Phase-12 validation API and the Phase-13 production session API are separate lifecycle surfaces. Application composition must not run them concurrently; explicit cross-surface exclusion can be hardened in the later robustness checkpoint.
