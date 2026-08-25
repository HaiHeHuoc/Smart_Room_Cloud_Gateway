# Voice Assistant

## Purpose

`voice_assistant` is the application orchestration layer for Phase 13. It is intentionally separate from `audio_manager`, `xiaozhi_foundation`, and `app_gui`.

Ownership is fixed as follows:

- `audio_manager` remains the sole production I2S/audio-hardware owner;
- `xiaozhi_foundation` remains the Xiaozhi transport/service boundary;
- `app_gui`/the LVGL UI task remain the only GUI owners;
- `voice_assistant` owns conversation state, session generation, and orchestration decisions only.

Phase 13-C adds the copied audio-status contract and formalizes `voice_assistant_status_t` as the UI-safe voice model. It still does **not** expose raw PCM, open a production audio channel, play response audio, or call LVGL.

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
    +--> LISTENING   (reserved for explicit voice-owned audio flow)
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

`CONNECTING -> READY` is not synthesized. `xiaozhi_foundation_session_start()` returns success only after the WebSocket chat reports a real CONNECTED event within its bounded timeout.

Phase 13-C deliberately does **not** transition to LISTENING/THINKING/SPEAKING merely because `audio_manager` reports RECORDING/PROCESSING/PLAYBACK. A local recording or WAV playback may be unrelated to the voice session. Those conversation transitions are activated only when a later phase gives `voice_assistant` explicit ownership of the voice command flow.

## Xiaozhi production session boundary

Phase 12 validation still exists separately. Phase 13-B does not reuse the P2-E/P2-F worker as a production session because those workers deliberately connect, validate, and tear down.

The long-lived foundation contract is:

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

## Audio contract

`audio_manager` already owns one application callback. Phase 13-C does not register a second callback owner. Instead it provides a small fan-out adapter:

```c
esp_err_t voice_assistant_audio_adapter_post(
    const audio_manager_status_t *status);
```

Application composition can call this from the existing audio-manager callback. The adapter copies only:

- lifecycle state;
- capture-active flag;
- playback-active flag;
- last `esp_err_t`.

It maps those facts into the project-owned `voice_assistant_audio_status_t` and then queues them through:

```c
esp_err_t voice_assistant_notify_audio_status(
    const voice_assistant_audio_status_t *status);
```

The voice task remains the only writer of the public `voice_assistant_status_t` snapshot.

The audio contract explicitly does **not** expose:

- I2S handles;
- DMA descriptors;
- manager-owned PCM buffers;
- private recording storage;
- raw hardware callbacks.

Current `audio_manager` does not provide a public live PCM frame/ring API. Phase 13-C therefore does not bypass private storage. A future microphone-to-Xiaozhi streaming path must first use a bounded project-owned public streaming contract while keeping `audio_manager` as the sole I2S owner.

## GUI contract

`voice_assistant_status_t` is now the production UI-safe contract. It contains only copied project-owned scalar state:

```text
conversation state
session generation
session-active flag
last voice error
copied audio state
capture/playback active flags
last audio error
```

A future `app_gui` adapter may queue this snapshot into the UI task. `voice_assistant` itself must never call LVGL or depend on LVGL object lifetime.

The Phase-12 `ui_xiaozhi_status_t` remains a temporary validation model and is **not** reused as the production Phase-13 contract merely to avoid adding the correct boundary. Production GUI rendering remains a later GUI phase.

## Task and queue

A single `voice_assistant` task owns state-machine and copied audio-status execution.

- stack: 4096 bytes;
- priority: 4;
- command queue length: 8;
- APIs use bounded mutex waits;
- task start waits at most 2 seconds for the task to reach `IDLE`;
- callbacks are invoked only after the component status lock has been released;
- audio status posting is non-blocking and returns `ESP_ERR_TIMEOUT` if the bounded queue is full.

`begin_session()` and `end_session()` enqueue bounded commands rather than performing transport work in the caller context. Transport setup/teardown executes in the orchestration task, not in Wi-Fi, Xiaozhi event-loop, audio-manager callback, or GUI callback context.

## Public API

```c
esp_err_t voice_assistant_init(void);
esp_err_t voice_assistant_start(void);
esp_err_t voice_assistant_begin_session(void);
esp_err_t voice_assistant_end_session(void);
esp_err_t voice_assistant_notify_audio_status(
    const voice_assistant_audio_status_t *status);
esp_err_t voice_assistant_register_status_callback(
    voice_assistant_status_callback_t callback,
    void *user_context);
esp_err_t voice_assistant_get_status(voice_assistant_status_t *status);
const char *voice_assistant_state_to_string(voice_assistant_state_t state);
const char *voice_assistant_audio_state_to_string(
    voice_assistant_audio_state_t state);
```

Composition-only audio adapter:

```c
esp_err_t voice_assistant_audio_adapter_post(
    const audio_manager_status_t *status);
```

## Current limitations / deferred work

Phase 13-C intentionally does not implement:

- live microphone PCM frame export from `audio_manager`;
- Opus TX or production Xiaozhi audio-channel open/close;
- response-audio routing into `audio_manager`;
- automatic LISTENING/THINKING/SPEAKING transitions from generic audio activity;
- USER/ASSISTANT transcript promotion into the production status contract;
- direct `app_gui` queue integration or Phase-15 voice rendering;
- automatic retry/recovery state machine after transport loss;
- production composition/start trigger in `main.c`;
- hardware/runtime acceptance.

The Phase-12 validation API and the Phase-13 production session API are separate lifecycle surfaces. Application composition must not run them concurrently; explicit cross-surface exclusion is hardened in the later robustness checkpoint.
