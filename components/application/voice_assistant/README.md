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

Phase 13 status: **Software Complete / HIL Pending**.

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
DISCONNECTED/SERVER_GOODBYE generated during explicit stop are treated as
teardown evidence instead of false runtime failures. A connection failure before
READY preserves `active=false`; an established-session failure preserves
`active=true` until recovery/cleanup.

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
after an intentional stop has already returned to IDLE is ignored rather than
regressing IDLE back to ERROR.

## Explicit recovery policy

`voice_assistant_recover()` is accepted only from `ERROR`:

```text
ERROR -> RECOVERING
      -> query foundation session status
      -> stop/cleanup once if still active
      -> IDLE on success
      -> ERROR on cleanup failure
```

There is no automatic reconnect loop. A later explicit `begin_session()` starts
a fresh generation.

## Audio contract

`voice_assistant_audio_adapter_post()` translates one existing copied
`audio_manager_status_t` into `voice_assistant_audio_status_t`. No I2S handle,
DMA descriptor, PCM pointer or private recording storage crosses the boundary.

Audio notifications use latest-value coalescing. While one audio marker is
pending in the command queue, newer audio snapshots overwrite the private copied
pending value instead of consuming additional queue slots.

The current `audio_manager` does not expose a public live PCM streaming API.
Phase 13 therefore does not bypass private audio storage. Real mic -> Opus ->
Xiaozhi uplink and response-audio playback are Sprint-14 work while
`audio_manager` remains the sole I2S owner.

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

## Production vs Phase-12 validation

The repository retains the Phase-12 validation composition and assets for HIL.
The Phase-13 production branch explicitly defaults:

```text
CONFIG_XIAOZHI_FOUNDATION_VALIDATION_ENABLE=n
```

so normal Phase-13 firmware does not automatically run the temporary validator.
Dedicated HIL/test branches may opt in explicitly.

Phase 13 also deliberately does not auto-call `voice_assistant_begin_session()`
at boot. A conversation must be user-authorized; Sprint 14 owns the PTT trigger,
voice-session start, mic uplink and response-audio lifecycle.

## Security boundary

Phase 13 exposes no voice API for reboot, OTA, NVS erase/write, Wi-Fi
reconfiguration, provisioning lifecycle, arbitrary GPIO/driver control or
shell/system commands.

Voice recovery may clean only the voice-owned Xiaozhi session. It may not take
over Wi-Fi, provisioning, cloud, reset, storage, audio hardware or GUI lifecycle.

## Verification boundary

Static/source review is complete. Final `idf.py build`, target runtime,
WebSocket/session HIL and resource measurements are not claimed in the current
environment and remain explicitly deferred.

Use `AI_Stored_Data/PHASE13_HIL_TEST_PLAN.md` when target hardware is available.
